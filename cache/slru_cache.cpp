﻿#include "slru_cache.hpp"
#include <cassert>

namespace ioremap { namespace cache {

// public:

slru_cache_t::slru_cache_t(struct dnet_node *n, const std::vector<size_t> &cache_pages_max_sizes) :
	m_need_exit(false),
	m_node(n),
	m_cache_pages_number(cache_pages_max_sizes.size()),
	m_cache_pages_max_sizes(cache_pages_max_sizes),
	m_cache_pages_sizes(m_cache_pages_number, 0),
	m_cache_pages_lru(new lru_list_t[m_cache_pages_number]) {
	m_lifecheck = std::thread(std::bind(&slru_cache_t::life_check, this));
}

slru_cache_t::~slru_cache_t() {
	stop();
	m_lifecheck.join();
	clear();
}

void slru_cache_t::stop() {
	m_need_exit = true;
}

int slru_cache_t::write(const unsigned char *id, dnet_net_state *st, dnet_cmd *cmd, dnet_io_attr *io, const char *data) {
	elliptics_timer timer;
	int result = write_(id, st, cmd, io, data);
	m_cache_stats.total_write_time += timer.elapsed<std::chrono::microseconds>();
	return result;
}

int slru_cache_t::write_(const unsigned char *id, dnet_net_state *st, dnet_cmd *cmd, dnet_io_attr *io, const char *data) {
	const size_t lifetime = io->start;
	const size_t size = io->size;
	const bool remove_from_disk = (io->flags & DNET_IO_FLAGS_CACHE_REMOVE_FROM_DISK);
	const bool cache = (io->flags & DNET_IO_FLAGS_CACHE);
	const bool cache_only = (io->flags & DNET_IO_FLAGS_CACHE_ONLY);
	const bool append = (io->flags & DNET_IO_FLAGS_APPEND);

	elliptics_timer timer;

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: before guard\n", dnet_dump_id_str(id));
	elliptics_unique_lock<std::mutex> guard(m_lock, m_node, "%s: CACHE WRITE: %p", dnet_dump_id_str(id), this);
	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: after guard, lock: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	data_t* it = m_treap.find(id);
	m_cache_stats.total_write_find_time += timer.restart<std::chrono::microseconds>();

	if (!it && !cache) {
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: not a cache call\n", dnet_dump_id_str(id));
		return -ENOTSUP;
	}

	// Optimization for append-only commands
	if (!cache_only) {
		if (append && (!it || it->only_append())) {
			bool new_page = false;
			if (!it) {
				it = create_data(id, 0, 0, false);
				new_page = true;
				it->set_only_append(true);
				size_t previous_eventtime = it->eventtime();
				it->set_synctime(time(NULL) + m_node->cache_sync_timeout);

				if (previous_eventtime != it->eventtime()) {
					m_treap.decrease_key(it);
				}
				m_cache_stats.total_write_create_data_time += timer.restart<std::chrono::microseconds>();
			}

			auto &raw = it->data()->data();
			size_t page_number = it->cache_page_number();
			size_t new_page_number = page_number;
			size_t new_size = it->size() + io->size;

			// Moving item to hotter page
			if (!new_page) {
				new_page_number = get_next_page_number(page_number);
			}

			remove_data_from_page(id, page_number, &*it);
			resize_page(id, new_page_number, 2 * new_size);
			m_cache_stats.total_write_resize_page_time += timer.restart<std::chrono::microseconds>();

			m_cache_stats.size_of_objects -= it->size();
			raw.insert(raw.end(), data, data + io->size);
			m_cache_stats.size_of_objects += it->size();

			insert_data_into_page(id, new_page_number, &*it);

			it->set_timestamp(io->timestamp);
			it->set_user_flags(io->user_flags);

			cmd->flags &= ~DNET_FLAGS_NEED_ACK;
			return dnet_send_file_info_ts_without_fd(st, cmd, data, io->size, &io->timestamp);
		} else if (it && it->only_append()) {
			sync_after_append(guard, false, &*it);

			dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: synced after append: %lld", dnet_dump_id_str(id), timer.restart());

			local_session sess(m_node);
			sess.set_ioflags(DNET_IO_FLAGS_NOCACHE | DNET_IO_FLAGS_APPEND);

			int err = m_node->cb->command_handler(st, m_node->cb->command_private, cmd, io);
			dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: second write result, cmd: %lld ms, err: %d", dnet_dump_id_str(id), timer.restart(), err);

			it = populate_from_disk(guard, id, false, &err);
			m_cache_stats.total_write_populate_from_disk_time += timer.restart<std::chrono::microseconds>();

			dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: read result, populate: %lld ms, err: %d", dnet_dump_id_str(id), timer.restart(), err);
			cmd->flags &= ~DNET_FLAGS_NEED_ACK;
			return err;
		}
	}

	bool new_page = false;

	if (!it) {
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: not exist\n", dnet_dump_id_str(id));
		// If file not found and CACHE flag is not set - fallback to backend request
		if (!cache_only && io->offset != 0) {
			int err = 0;
			it = populate_from_disk(guard, id, remove_from_disk, &err);
			m_cache_stats.total_write_populate_from_disk_time += timer.restart<std::chrono::microseconds>();
			new_page = true;

			if (err != 0 && err != -ENOENT)
				return err;
		}

		// Create empty data for code simplifyng
		if (!it) {
			it = create_data(id, 0, 0, remove_from_disk);
			m_cache_stats.total_write_create_data_time += timer.restart<std::chrono::microseconds>();
			new_page = true;
		}
	} else {
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: exists\n", dnet_dump_id_str(id));
	}
	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: data ensured: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	raw_data_t &raw = *it->data();

	if (io->flags & DNET_IO_FLAGS_COMPARE_AND_SWAP) {
		// Data is already in memory, so it's free to use it
		// raw.size() is zero only if there is no such file on the server
		if (raw.size() != 0) {
			struct dnet_raw_id csum;
			dnet_transform_node(m_node, raw.data().data(), raw.size(), csum.id, sizeof(csum.id));

			if (memcmp(csum.id, io->parent, DNET_ID_SIZE)) {
				dnet_log(m_node, DNET_LOG_ERROR, "%s: cas: cache checksum mismatch\n", dnet_dump_id(&cmd->id));
				return -EBADFD;
			}
		}
	}

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: CAS checked: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	size_t new_data_size = 0;

	if (append) {
		new_data_size = raw.size() + size;
	} else {
		new_data_size = io->offset + io->size;
	}

	size_t new_size = new_data_size + it->overhead_size();

	size_t page_number = it->cache_page_number();
	size_t new_page_number = page_number;

	if (!new_page) {
		new_page_number = get_next_page_number(page_number);
	}

	remove_data_from_page(id, page_number, &*it);
	timer.restart();
	resize_page(id, new_page_number, new_size);
	m_cache_stats.total_write_resize_page_time += timer.restart<std::chrono::microseconds>();

	m_cache_stats.size_of_objects -= it->size();
	if (append) {
		raw.data().insert(raw.data().end(), data, data + size);
	} else {
		raw.data().resize(new_data_size);
		memcpy(raw.data().data() + io->offset, data, size);
	}
	m_cache_stats.size_of_objects += it->size();

	it->set_remove_from_cache(false);
	insert_data_into_page(id, new_page_number, &*it);

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: data modified: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	// Mark data as dirty one, so it will be synced to the disk

	size_t previous_eventtime = it->eventtime();

	if (!it->synctime() && !(io->flags & DNET_IO_FLAGS_CACHE_ONLY)) {
		it->set_synctime(time(NULL) + m_node->cache_sync_timeout);
	}

	if (lifetime) {
		it->set_lifetime(lifetime + time(NULL));
	}

	if (previous_eventtime != it->eventtime()) {
		m_treap.decrease_key(it);
	}

	it->set_timestamp(io->timestamp);
	it->set_user_flags(io->user_flags);

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: finished write: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	cmd->flags &= ~DNET_FLAGS_NEED_ACK;
	return dnet_send_file_info_ts_without_fd(st, cmd, raw.data().data() + io->offset, io->size, &io->timestamp);
}

std::shared_ptr<raw_data_t> slru_cache_t::read(const unsigned char *id, dnet_cmd *cmd, dnet_io_attr *io) {
	elliptics_timer timer;
	auto result = read_(id, cmd, io);
	m_cache_stats.total_read_time += timer.elapsed<std::chrono::microseconds>();
	return result;
}

std::shared_ptr<raw_data_t> slru_cache_t::read_(const unsigned char *id, dnet_cmd *cmd, dnet_io_attr *io) {
	const bool cache = (io->flags & DNET_IO_FLAGS_CACHE);
	const bool cache_only = (io->flags & DNET_IO_FLAGS_CACHE_ONLY);
	(void) cmd;

	elliptics_timer timer;

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE READ: before guard\n", dnet_dump_id_str(id));
	elliptics_unique_lock<std::mutex> guard(m_lock, m_node, "%s: CACHE READ: %p", dnet_dump_id_str(id), this);
	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE READ: after guard, lock: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	bool new_page = false;

	data_t* it = m_treap.find(id);
	if (it && it->only_append()) {
		sync_after_append(guard, true, &*it);
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE READ: synced append-only data, find+sync: %lld ms\n", dnet_dump_id_str(id), timer.restart());

		it = NULL;
	}
	timer.restart();

	if (!it && cache && !cache_only) {
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE READ: not exist\n", dnet_dump_id_str(id));
		int err = 0;
		it = populate_from_disk(guard, id, false, &err);
		new_page = true;
	} else {
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE READ: exists\n", dnet_dump_id_str(id));
	}

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE READ: data ensured: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	if (it) {

		size_t page_number = it->cache_page_number();
		size_t new_page_number = page_number;

		it->set_remove_from_cache(false);

		if (!new_page) {
			new_page_number = get_next_page_number(page_number);
		}

		move_data_between_pages(id, page_number, new_page_number, &*it);

		io->timestamp = it->timestamp();
		io->user_flags = it->user_flags();
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE READ: returned: %lld ms\n", dnet_dump_id_str(id), timer.restart());
		return it->data();
	}

	return std::shared_ptr<raw_data_t>();
}

int slru_cache_t::remove(const unsigned char *id, dnet_io_attr *io) {
	elliptics_timer timer;
	int result = remove_(id, io);
	m_cache_stats.total_remove_time += timer.elapsed<std::chrono::microseconds>();
	return result;
}

int slru_cache_t::remove_(const unsigned char *id, dnet_io_attr *io) {
	const bool cache_only = (io->flags & DNET_IO_FLAGS_CACHE_ONLY);
	bool remove_from_disk = !cache_only;
	int err = -ENOENT;

	elliptics_timer timer;

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE REMOVE: before guard\n", dnet_dump_id_str(id));
	elliptics_unique_lock<std::mutex> guard(m_lock, m_node, "%s: CACHE REMOVE: %p", dnet_dump_id_str(id), this);
	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE REMOVE: after guard, lock: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	data_t* it = m_treap.find(id);
	if (it) {
		// If cache_only is not set the data also should be remove from the disk
		// If data is marked and cache_only is not set - data must be synced to the disk
		remove_from_disk |= it->remove_from_disk();
		if (it->synctime() && !cache_only) {
			size_t previous_eventtime = it->eventtime();
			it->clear_synctime();

			if (previous_eventtime != it->eventtime()) {
				m_treap.decrease_key(it);
			}
		}
		erase_element(&(*it));
		err = 0;
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE REMOVE: erased: %lld ms\n", dnet_dump_id_str(id), timer.restart());
	}

	guard.unlock();

	if (remove_from_disk) {
		struct dnet_id raw;
		memset(&raw, 0, sizeof(struct dnet_id));

		dnet_setup_id(&raw, 0, (unsigned char *)id);

		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE REMOVE: before removing from disk\n", dnet_dump_id_str(id));
		timer.restart();
		int local_err = dnet_remove_local(m_node, &raw);
		if (local_err != -ENOENT)
			err = local_err;
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE REMOVE: after removing from disk: %lld ms\n", dnet_dump_id_str(id), timer.restart());
	}

	return err;
}

int slru_cache_t::lookup(const unsigned char *id, dnet_net_state *st, dnet_cmd *cmd) {
	elliptics_timer timer;
	int result = lookup_(id, st, cmd);
	m_cache_stats.total_lookup_time += timer.elapsed<std::chrono::microseconds>();
	return result;
}

void slru_cache_t::clear()
{
	std::vector<size_t> cache_pages_max_sizes = m_cache_pages_max_sizes;

	elliptics_unique_lock<std::mutex> guard(m_lock, m_node, "CACHE CLEAR: %p", this);

	for (size_t page_number = 0; page_number < m_cache_pages_number; ++page_number) {
		m_cache_pages_max_sizes[page_number] = 0;
		resize_page((unsigned char *) "", page_number, 0);
	}

	while (!m_treap.empty()) {
		erase_element(m_treap.top());
	}

	m_cache_pages_max_sizes = cache_pages_max_sizes;
}

int slru_cache_t::lookup_(const unsigned char *id, dnet_net_state *st, dnet_cmd *cmd) {
	int err = 0;

	elliptics_timer timer;

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE LOOKUP: before guard\n", dnet_dump_id_str(id));
	elliptics_unique_lock<std::mutex> guard(m_lock, m_node, "%s: CACHE LOOKUP: %p", dnet_dump_id_str(id), this);
	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE LOOKUP: after guard, lock: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	data_t* it = m_treap.find(id);
	if (!it) {
		return -ENOTSUP;
	}

	dnet_time timestamp = it->timestamp();

	guard.unlock();

	local_session sess(m_node);

	cmd->flags |= DNET_FLAGS_NOCACHE;

	ioremap::elliptics::data_pointer data = sess.lookup(*cmd, &err);

	cmd->flags &= ~DNET_FLAGS_NOCACHE;

	if (err) {
		cmd->flags &= ~DNET_FLAGS_NEED_ACK;
		return dnet_send_file_info_ts_without_fd(st, cmd, NULL, 0, &timestamp);
	}

	dnet_file_info *info = data.skip<dnet_addr>().data<dnet_file_info>();
	info->mtime = timestamp;

	cmd->flags &= (DNET_FLAGS_MORE | DNET_FLAGS_NEED_ACK);
	return dnet_send_reply(st, cmd, data.data(), data.size(), 0);
}

cache_stats slru_cache_t::get_cache_stats() const
{
	cache_stats stats(m_cache_stats);
	stats.pages_sizes = m_cache_pages_sizes;
	stats.pages_max_sizes = m_cache_pages_max_sizes;
	return stats;
}

// private:

void slru_cache_t::insert_data_into_page(const unsigned char *id, size_t page_number, data_t *data)
{
	elliptics_timer timer;
	size_t size = data->size();

	// Recalc used space, free enough space for new data, move object to the end of the queue
	if (m_cache_pages_sizes[page_number] + size > m_cache_pages_max_sizes[page_number]) {
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: resize called: %lld ms\n", dnet_dump_id_str(id), timer.restart());
		resize_page(id, page_number, size);
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: resize finished: %lld ms\n", dnet_dump_id_str(id), timer.restart());
	}

	data->set_cache_page_number(page_number);
	m_cache_pages_lru[page_number].push_back(*data);
	m_cache_pages_sizes[page_number] += size;
}

void slru_cache_t::remove_data_from_page(const unsigned char *id, size_t page_number, data_t *data)
{
	(void) id;
	m_cache_pages_sizes[page_number] -= data->size();
	m_cache_pages_lru[page_number].erase(m_cache_pages_lru[page_number].iterator_to(*data));
}

void slru_cache_t::move_data_between_pages(const unsigned char *id, size_t source_page_number, size_t destination_page_number, data_t *data)
{
	if (source_page_number != destination_page_number) {
		remove_data_from_page(id, source_page_number, data);
		insert_data_into_page(id, destination_page_number, data);
	}
}

data_t* slru_cache_t::create_data(const unsigned char *id, const char *data, size_t size, bool remove_from_disk) {
	size_t last_page_number = m_cache_pages_number - 1;

	data_t *raw = new data_t(id, 0, data, size, remove_from_disk);

	insert_data_into_page(id, last_page_number, raw);

	m_cache_stats.number_of_objects++;
	m_cache_stats.size_of_objects += raw->size();
	m_treap.insert(raw);
	return raw;
}

data_t* slru_cache_t::populate_from_disk(elliptics_unique_lock<std::mutex> &guard, const unsigned char *id, bool remove_from_disk, int *err) {
	if (guard.owns_lock()) {
		guard.unlock();
	}

	elliptics_timer timer;

	local_session sess(m_node);
	sess.set_ioflags(DNET_IO_FLAGS_NOCACHE);

	dnet_id raw_id;
	memset(&raw_id, 0, sizeof(raw_id));
	memcpy(raw_id.id, id, DNET_ID_SIZE);

	uint64_t user_flags = 0;
	dnet_time timestamp;
	dnet_empty_time(&timestamp);

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: populating from disk started: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	ioremap::elliptics::data_pointer data = sess.read(raw_id, &user_flags, &timestamp, err);

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: populating from disk finished: %lld ms, err: %d\n", dnet_dump_id_str(id), timer.restart(), *err);

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: populating from disk, before lock: %lld ms\n", dnet_dump_id_str(id), timer.restart());
	guard.lock();
	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: populating from disk, after lock: %lld ms\n", dnet_dump_id_str(id), timer.restart());

	if (*err == 0) {
		auto it = create_data(id, reinterpret_cast<char *>(data.data()), data.size(), remove_from_disk);
		it->set_user_flags(user_flags);
		it->set_timestamp(timestamp);
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: populating from disk, data created: %lld ms\n", dnet_dump_id_str(id), timer.restart());
		return it;
	}

	return NULL;
}

bool slru_cache_t::have_enough_space(const unsigned char *id, size_t page_number, size_t reserve) {
	(void) id;
	return m_cache_pages_max_sizes[page_number] >= reserve;
}

void slru_cache_t::resize_page(const unsigned char *id, size_t page_number, size_t reserve) {
	elliptics_timer timer;
	size_t removed_size = 0;
	size_t &cache_size = m_cache_pages_sizes[page_number];
	size_t &max_cache_size = m_cache_pages_max_sizes[page_number];
	size_t previous_page_number = get_previous_page_number(page_number);

	for (auto it = m_cache_pages_lru[page_number].begin(); it != m_cache_pages_lru[page_number].end();) {
		if (max_cache_size + removed_size >= cache_size + reserve)
			break;

		data_t *raw = &*it;
		++it;

		// If page is not last move object to previous page
		if (previous_page_number < m_cache_pages_number) {
			move_data_between_pages(id, page_number, previous_page_number, raw);
		} else {
			if (raw->synctime() || raw->remove_from_cache()) {
				if (!raw->remove_from_cache()) {
					m_cache_stats.number_of_objects_marked_for_deletion++;
					m_cache_stats.size_of_objects_marked_for_deletion += raw->size();
					raw->set_remove_from_cache(true);

					size_t previous_eventtime = raw->eventtime();
					raw->set_synctime(1);
					if (previous_eventtime != raw->eventtime()) {
						m_treap.decrease_key(raw);
					}
				}
				removed_size += raw->size();
			} else {
				erase_element(raw);
			}
		}
	}
	m_cache_stats.total_resize_time += timer.elapsed<std::chrono::microseconds>();
}

void slru_cache_t::erase_element(data_t *obj) {
	elliptics_timer timer;

	m_cache_stats.number_of_objects--;
	m_cache_stats.size_of_objects -= obj->size();

	size_t page_number = obj->cache_page_number();
	m_cache_pages_sizes[page_number] -= obj->size();
	m_cache_pages_lru[page_number].erase(m_cache_pages_lru[page_number].iterator_to(*obj));
	m_treap.erase(obj);

	if (obj->eventtime()) {
		if (obj->synctime()) {
			sync_element(obj);
			obj->clear_synctime();
		}
	}

	if (obj->remove_from_cache())
	{
		m_cache_stats.number_of_objects_marked_for_deletion--;
		m_cache_stats.size_of_objects_marked_for_deletion -= obj->size();
	}

	dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: erased element: %lld ms\n", dnet_dump_id_str(obj->id().id), timer.restart());

	delete obj;
}

void slru_cache_t::sync_element(const dnet_id &raw, bool after_append, const std::vector<char> &data, uint64_t user_flags, const dnet_time &timestamp) {
	local_session sess(m_node);
	sess.set_ioflags(DNET_IO_FLAGS_NOCACHE | (after_append ? DNET_IO_FLAGS_APPEND : 0));

	int err = sess.write(raw, data.data(), data.size(), user_flags, timestamp);
	if (err) {
		dnet_log(m_node, DNET_LOG_ERROR, "%s: CACHE: forced to sync to disk, err: %d\n", dnet_dump_id_str(raw.id), err);
	} else {
		dnet_log(m_node, DNET_LOG_DEBUG, "%s: CACHE: forced to sync to disk, err: %d\n", dnet_dump_id_str(raw.id), err);
	}
}

void slru_cache_t::sync_element(data_t *obj) {
	struct dnet_id raw;
	memset(&raw, 0, sizeof(struct dnet_id));
	memcpy(raw.id, obj->id().id, DNET_ID_SIZE);

	auto &data = obj->data()->data();

	sync_element(raw, obj->only_append(), data, obj->user_flags(), obj->timestamp());
}

void slru_cache_t::sync_after_append(elliptics_unique_lock<std::mutex> &guard, bool lock_guard, data_t *obj) {
	elliptics_timer timer;

	std::shared_ptr<raw_data_t> raw_data = obj->data();

	obj->clear_synctime();

	dnet_id id;
	memset(&id, 0, sizeof(id));
	memcpy(id.id, obj->id().id, DNET_ID_SIZE);

	uint64_t user_flags = obj->user_flags();
	dnet_time timestamp = obj->timestamp();

	const auto timer_prepare = timer.restart();

	erase_element(&*obj);

	const auto timer_erase = timer.restart();

	guard.unlock();

	local_session sess(m_node);
	sess.set_ioflags(DNET_IO_FLAGS_NOCACHE | DNET_IO_FLAGS_APPEND);

	auto &raw = raw_data->data();

	const auto timer_before_write = timer.restart();

	int err = sess.write(id, raw.data(), raw.size(), user_flags, timestamp);

	const auto timer_after_write = timer.restart();

	if (lock_guard)
		guard.lock();

	const auto timer_lock = timer.restart();

	dnet_log(m_node, DNET_LOG_INFO, "%s: CACHE: sync after append, "
			 "prepare: %lld ms, erase: %lld ms, before_write: %lld ms, after_write: %lld ms, lock: %lld ms, err: %d",
			 dnet_dump_id_str(id.id), timer_prepare, timer_erase, timer_before_write, timer_after_write, timer_lock, err);
}

struct record_info {
	record_info(data_t* obj)
	{
		only_append = obj->only_append();
		memcpy(id.id, obj->id().id, DNET_ID_SIZE);
		data = obj->data()->data();
		user_flags = obj->user_flags();
		timestamp = obj->timestamp();
	}

	bool only_append;
	dnet_id id;
	std::vector<char> data;
	uint64_t user_flags;
	dnet_time timestamp;
};

void slru_cache_t::life_check(void) {
	elliptics_timer lifecheck_timer;
	while (!m_need_exit) {
		(void) lifecheck_timer.restart();
		std::deque<struct dnet_id> remove;
		std::deque<record_info> elements_for_sync;

		{
			elliptics_unique_lock<std::mutex> guard(m_lock, m_node, "CACHE LIFE: %p", this);

			while (!m_need_exit && !m_treap.empty()) {
				size_t time = ::time(NULL);

				if (m_treap.empty())
					break;

				data_t* it = m_treap.top();
				if (it->eventtime() > time)
					break;

				if (it->eventtime() == it->lifetime())
				{
					if (it->remove_from_disk()) {
						struct dnet_id id;
						memset(&id, 0, sizeof(struct dnet_id));

						dnet_setup_id(&id, 0, (unsigned char *)it->id().id);

						remove.push_back(id);
					}

					erase_element(&(*it));
				}
				else if (it->eventtime() == it->synctime())
				{
					elements_for_sync.push_back(record_info(&*it));

					it->clear_synctime();

					if (it->only_append() || it->remove_from_cache()) {
						erase_element(&*it);
					}
				}
			}
		}
		for (auto it = elements_for_sync.begin(); it != elements_for_sync.end(); ++it) {
			dnet_oplock(m_node, &it->id);

			// sync_element uses local_session which always uses DNET_FLAGS_NOLOCK
			sync_element(it->id, it->only_append, it->data, it->user_flags, it->timestamp);

			dnet_opunlock(m_node, &it->id);
		}
		for (std::deque<struct dnet_id>::iterator it = remove.begin(); it != remove.end(); ++it) {
			dnet_remove_local(m_node, &(*it));
		}

		m_cache_stats.total_lifecheck_time += lifecheck_timer.elapsed<std::chrono::microseconds>();
		sleep(1);
	}
}

}}
