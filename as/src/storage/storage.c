/*
 * storage.c
 *
 * Copyright (C) 2009-2016 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

//==========================================================
// Includes.
//

#include "storage/storage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "citrusleaf/cf_digest.h"
#include "citrusleaf/cf_queue.h"

#include "cf_mutex.h"
#include "fault.h"
#include "olock.h"

#include "base/cfg.h"
#include "base/datamodel.h"
#include "base/index.h"
#include "base/rec_props.h"
#include "base/thr_info.h"
#include "fabric/partition.h"


//==========================================================
// Generic "base class" functions that call through
// storage-engine "v-tables".
//

//--------------------------------------
// as_storage_init
//

typedef int (*as_storage_namespace_init_fn)(as_namespace *ns, cf_queue *complete_q, void *udata);
static const as_storage_namespace_init_fn as_storage_namespace_init_table[AS_NUM_STORAGE_ENGINES] = {
	as_storage_namespace_init_memory,
	as_storage_namespace_init_ssd
};

void
as_storage_init()
{
	cf_queue *complete_q = cf_queue_create(sizeof(void*), true);

	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		as_namespace *ns = g_config.namespaces[i];

		if (as_storage_namespace_init_table[ns->storage_type]) {
			if (0 != as_storage_namespace_init_table[ns->storage_type](ns, complete_q, NULL)) {
				cf_crash(AS_STORAGE, "could not initialize storage for namespace %s", ns->name);
			}
		}
		else {
			cf_crash(AS_STORAGE, "invalid storage type for namespace %s", ns->name);
		}
	}

	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		void *_t;

		while (CF_QUEUE_OK != cf_queue_pop(complete_q, &_t, 2000)) {
			as_storage_loading_records_ticker_ssd();
		}
	}

	cf_queue_destroy(complete_q);
}

//--------------------------------------
// as_storage_start_tomb_raider
//

typedef void (*as_storage_start_tomb_raider_fn)(as_namespace *ns);
static const as_storage_start_tomb_raider_fn as_storage_start_tomb_raider_table[AS_NUM_STORAGE_ENGINES] = {
	as_storage_start_tomb_raider_memory,
	as_storage_start_tomb_raider_ssd
};

void
as_storage_start_tomb_raider()
{
	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		as_namespace *ns = g_config.namespaces[i];

		if (as_storage_start_tomb_raider_table[ns->storage_type]) {
			as_storage_start_tomb_raider_table[ns->storage_type](ns);
		}
	}
}

//--------------------------------------
// as_storage_namespace_destroy
//

typedef int (*as_storage_namespace_destroy_fn)(as_namespace *ns);
static const as_storage_namespace_destroy_fn as_storage_namespace_destroy_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no destroy
	as_storage_namespace_destroy_ssd
};

int
as_storage_namespace_destroy(as_namespace *ns)
{
	if (as_storage_namespace_destroy_table[ns->storage_type]) {
		return as_storage_namespace_destroy_table[ns->storage_type](ns);
	}

	return 0;
}

//--------------------------------------
// as_storage_record_destroy
//

typedef int (*as_storage_record_destroy_fn)(as_namespace *ns, as_record *r);
static const as_storage_record_destroy_fn as_storage_record_destroy_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no record destroy
	as_storage_record_destroy_ssd
};

int
as_storage_record_destroy(as_namespace *ns, as_record *r)
{
	if (as_storage_record_destroy_table[ns->storage_type]) {
		return as_storage_record_destroy_table[ns->storage_type](ns, r);
	}

	return 0;
}

//--------------------------------------
// as_storage_record_create
//

typedef int (*as_storage_record_create_fn)(as_storage_rd *rd);
static const as_storage_record_create_fn as_storage_record_create_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no record create
	as_storage_record_create_ssd
};

int
as_storage_record_create(as_namespace *ns, as_record *r, as_storage_rd *rd)
{
	rd->r = r;
	rd->ns = ns;
	as_rec_props_clear(&rd->rec_props);
	rd->bins = 0;
	rd->n_bins = 0;
	rd->record_on_device = false;
	rd->ignore_record_on_device = false;
	rd->key_size = 0;
	rd->key = NULL;
	rd->is_durable_delete = false;

	if (as_storage_record_create_table[ns->storage_type]) {
		return as_storage_record_create_table[ns->storage_type](rd);
	}

	return 0;
}

//--------------------------------------
// as_storage_record_open
//

typedef int (*as_storage_record_open_fn)(as_storage_rd *rd);
static const as_storage_record_open_fn as_storage_record_open_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no record open
	as_storage_record_open_ssd
};

int
as_storage_record_open(as_namespace *ns, as_record *r, as_storage_rd *rd)
{
	rd->r = r;
	rd->ns = ns;
	as_rec_props_clear(&rd->rec_props);
	rd->bins = 0;
	rd->n_bins = 0;
	rd->record_on_device = true;
	rd->ignore_record_on_device = false;
	rd->key_size = 0;
	rd->key = NULL;
	rd->is_durable_delete = false;

	if (as_storage_record_open_table[ns->storage_type]) {
		return as_storage_record_open_table[ns->storage_type](rd);
	}

	return 0;
}

//--------------------------------------
// as_storage_record_close
//

typedef int (*as_storage_record_close_fn)(as_storage_rd *rd);
static const as_storage_record_close_fn as_storage_record_close_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no record close
	as_storage_record_close_ssd
};

int
as_storage_record_close(as_storage_rd *rd)
{
	if (as_storage_record_close_table[rd->ns->storage_type]) {
		return as_storage_record_close_table[rd->ns->storage_type](rd);
	}

	return 0;
}

//--------------------------------------
// as_storage_record_load_n_bins
//

typedef int (*as_storage_record_load_n_bins_fn)(as_storage_rd *rd);
static const as_storage_record_load_n_bins_fn as_storage_record_load_n_bins_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no record load n bins
	as_storage_record_load_n_bins_ssd
};

int
as_storage_record_load_n_bins(as_storage_rd *rd)
{
	if (as_storage_record_load_n_bins_table[rd->ns->storage_type]) {
		return as_storage_record_load_n_bins_table[rd->ns->storage_type](rd);
	}

	return 0;
}

//--------------------------------------
// as_storage_record_load_bins
//

typedef int (*as_storage_record_load_bins_fn)(as_storage_rd *rd);
static const as_storage_record_load_bins_fn as_storage_record_load_bins_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no record load bins
	as_storage_record_load_bins_ssd
};

int
as_storage_record_load_bins(as_storage_rd *rd)
{
	if (as_storage_record_load_bins_table[rd->ns->storage_type]) {
		return as_storage_record_load_bins_table[rd->ns->storage_type](rd);
	}

	return 0;
}

//--------------------------------------
// as_storage_record_size_and_check
//

typedef bool (*as_storage_record_size_and_check_fn)(as_storage_rd *rd);
static const as_storage_record_size_and_check_fn as_storage_record_size_and_check_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // no limit if no persistent storage - flat size is irrelevant
	as_storage_record_size_and_check_ssd
};

bool
as_storage_record_size_and_check(as_storage_rd *rd)
{
	if (as_storage_record_size_and_check_table[rd->ns->storage_type]) {
		return as_storage_record_size_and_check_table[rd->ns->storage_type](rd);
	}

	return true;
}

//--------------------------------------
// as_storage_record_write
//

typedef int (*as_storage_record_write_fn)(as_storage_rd *rd);
static const as_storage_record_write_fn as_storage_record_write_table[AS_NUM_STORAGE_ENGINES] = {
	as_storage_record_write_memory,
	as_storage_record_write_ssd
};

int
as_storage_record_write(as_storage_rd *rd)
{
	if (as_storage_record_write_table[rd->ns->storage_type]) {
		return as_storage_record_write_table[rd->ns->storage_type](rd);
	}

	return 0;
}

//--------------------------------------
// as_storage_wait_for_defrag
//

typedef void (*as_storage_wait_for_defrag_fn)(as_namespace *ns);
static const as_storage_wait_for_defrag_fn as_storage_wait_for_defrag_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory doesn't do defrag
	as_storage_wait_for_defrag_ssd
};

void
as_storage_wait_for_defrag()
{
	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		as_namespace *ns = g_config.namespaces[i];

		if (as_storage_wait_for_defrag_table[ns->storage_type]) {
			as_storage_wait_for_defrag_table[ns->storage_type](ns);
		}
	}
}

//--------------------------------------
// as_storage_overloaded
//

typedef bool (*as_storage_overloaded_fn)(as_namespace *ns);
static const as_storage_overloaded_fn as_storage_overloaded_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no overload check
	as_storage_overloaded_ssd
};

bool
as_storage_overloaded(as_namespace *ns)
{
	if (as_storage_overloaded_table[ns->storage_type]) {
		return as_storage_overloaded_table[ns->storage_type](ns);
	}

	return false;
}

//--------------------------------------
// as_storage_has_space
//

typedef bool (*as_storage_has_space_fn)(as_namespace *ns);
static const as_storage_has_space_fn as_storage_has_space_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory has no space check
	as_storage_has_space_ssd
};

bool
as_storage_has_space(as_namespace *ns)
{
	if (as_storage_has_space_table[ns->storage_type]) {
		return as_storage_has_space_table[ns->storage_type](ns);
	}

	return true;
}

//--------------------------------------
// as_storage_defrag_sweep
//

typedef void (*as_storage_defrag_sweep_fn)(as_namespace *ns);
static const as_storage_defrag_sweep_fn as_storage_defrag_sweep_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory doesn't do defrag
	as_storage_defrag_sweep_ssd
};

void
as_storage_defrag_sweep(as_namespace *ns)
{
	if (as_storage_defrag_sweep_table[ns->storage_type]) {
		as_storage_defrag_sweep_table[ns->storage_type](ns);
	}
}

//--------------------------------------
// as_storage_info_set
//

typedef void (*as_storage_info_set_fn)(as_namespace *ns, const as_partition *p, bool flush);
static const as_storage_info_set_fn as_storage_info_set_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory doesn't support info
	as_storage_info_set_ssd
};

void
as_storage_info_set(as_namespace *ns, const as_partition *p, bool flush)
{
	if (as_storage_info_set_table[ns->storage_type]) {
		as_storage_info_set_table[ns->storage_type](ns, p, flush);
	}
}

//--------------------------------------
// as_storage_info_get
//

typedef void (*as_storage_info_get_fn)(as_namespace *ns, as_partition *p);
static const as_storage_info_get_fn as_storage_info_get_table[AS_NUM_STORAGE_ENGINES] = {
	as_storage_info_get_memory,
	as_storage_info_get_ssd
};

void
as_storage_info_get(as_namespace *ns, as_partition *p)
{
	if (as_storage_info_get_table[ns->storage_type]) {
		as_storage_info_get_table[ns->storage_type](ns, p);
	}
}

//--------------------------------------
// as_storage_info_flush
//

typedef int (*as_storage_info_flush_fn)(as_namespace *ns);
static const as_storage_info_flush_fn as_storage_info_flush_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory doesn't support info
	as_storage_info_flush_ssd
};

int
as_storage_info_flush(as_namespace *ns)
{
	if (as_storage_info_flush_table[ns->storage_type]) {
		return as_storage_info_flush_table[ns->storage_type](ns);
	}

	return 0;
}

//--------------------------------------
// as_storage_save_evict_void_time
//

typedef void (*as_storage_save_evict_void_time_fn)(as_namespace *ns, uint32_t evict_void_time);
static const as_storage_save_evict_void_time_fn as_storage_save_evict_void_time_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory doesn't store info
	as_storage_save_evict_void_time_ssd
};

void
as_storage_save_evict_void_time(as_namespace *ns, uint32_t evict_void_time)
{
	if (as_storage_save_evict_void_time_table[ns->storage_type]) {
		as_storage_save_evict_void_time_table[ns->storage_type](ns, evict_void_time);
	}
}

//--------------------------------------
// as_storage_stats
//

typedef int (*as_storage_stats_fn)(as_namespace *ns, int *available_pct, uint64_t *used_disk_bytes);
static const as_storage_stats_fn as_storage_stats_table[AS_NUM_STORAGE_ENGINES] = {
	as_storage_stats_memory,
	as_storage_stats_ssd
};

int
as_storage_stats(as_namespace *ns, int *available_pct, uint64_t *used_disk_bytes)
{
	if (as_storage_stats_table[ns->storage_type]) {
		return as_storage_stats_table[ns->storage_type](ns, available_pct, used_disk_bytes);
	}

	return 0;
}

//--------------------------------------
// as_storage_ticker_stats
//

typedef int (*as_storage_ticker_stats_fn)(as_namespace *ns);
static const as_storage_ticker_stats_fn as_storage_ticker_stats_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory doesn't support per-disk histograms... for now.
	as_storage_ticker_stats_ssd
};

int
as_storage_ticker_stats(as_namespace *ns)
{
	if (as_storage_ticker_stats_table[ns->storage_type]) {
		return as_storage_ticker_stats_table[ns->storage_type](ns);
	}

	return 0;
}

//--------------------------------------
// as_storage_histogram_clear_all
//

typedef int (*as_storage_histogram_clear_fn)(as_namespace *ns);
static const as_storage_histogram_clear_fn as_storage_histogram_clear_table[AS_NUM_STORAGE_ENGINES] = {
	NULL, // memory doesn't support per-disk histograms... for now.
	as_storage_histogram_clear_ssd
};

int
as_storage_histogram_clear_all(as_namespace *ns)
{
	if (as_storage_histogram_clear_table[ns->storage_type]) {
		return as_storage_histogram_clear_table[ns->storage_type](ns);
	}

	return 0;
}


//==========================================================
// Generic functions that don't use "v-tables".
//

// Get size of record's in-memory data - everything except index bytes.
uint64_t
as_storage_record_get_n_bytes_memory(as_storage_rd *rd)
{
	if (! rd->ns->storage_data_in_memory) {
		return 0;
	}

	uint64_t n_bytes_memory = 0;

	for (uint16_t i = 0; i < rd->n_bins; i++) {
		n_bytes_memory += as_bin_particle_size(&rd->bins[i]);
	}

	if (! rd->ns->single_bin) {
		if (rd->r->key_stored == 1) {
			n_bytes_memory += sizeof(as_rec_space) +
					((as_rec_space*)rd->r->dim)->key_size;
		}

		if (as_index_get_bin_space(rd->r)) {
			n_bytes_memory += sizeof(as_bin_space) +
					(sizeof(as_bin) * rd->n_bins);
		}
	}

	return n_bytes_memory;
}

void
as_storage_record_adjust_mem_stats(as_storage_rd *rd, uint64_t start_bytes)
{
	if (! rd->ns->storage_data_in_memory) {
		return;
	}

	uint64_t end_bytes = as_storage_record_get_n_bytes_memory(rd);
	int64_t delta_bytes = (int64_t)end_bytes - (int64_t)start_bytes;

	if (delta_bytes != 0) {
		cf_atomic_int_add(&rd->ns->n_bytes_memory, delta_bytes);
		as_namespace_adjust_set_memory(rd->ns, as_index_get_set_id(rd->r),
				delta_bytes);
	}
}

void
as_storage_record_drop_from_mem_stats(as_storage_rd *rd)
{
	if (! rd->ns->storage_data_in_memory) {
		return;
	}

	uint64_t drop_bytes = as_storage_record_get_n_bytes_memory(rd);

	cf_atomic_int_sub(&rd->ns->n_bytes_memory, drop_bytes);
	as_namespace_adjust_set_memory(rd->ns, as_index_get_set_id(rd->r),
			-(int64_t)drop_bytes);
}

bool
as_storage_record_get_key(as_storage_rd *rd)
{
	if (rd->r->key_stored == 0) {
		return false;
	}

	if (rd->ns->storage_data_in_memory) {
		rd->key_size = ((as_rec_space*)rd->r->dim)->key_size;
		rd->key = ((as_rec_space*)rd->r->dim)->key;
		return true;
	}

	if (rd->record_on_device && ! rd->ignore_record_on_device) {
		return as_storage_record_get_key_ssd(rd);
	}

	return false;
}

size_t
as_storage_record_rec_props_size(as_storage_rd *rd)
{
	size_t rec_props_data_size = 0;

	const char *set_name = as_index_get_set_name(rd->r, rd->ns);

	if (set_name) {
		rec_props_data_size += as_rec_props_sizeof_field(strlen(set_name) + 1);
	}

	if (rd->key) {
		rec_props_data_size += as_rec_props_sizeof_field(rd->key_size);
	}

	return rec_props_data_size;
}

// Populates rec_props struct in rd, using index info where possible. Assumes
// relevant information is ready:
// - set name
// - record key
// Relies on caller's properly allocated rec_props_data.
void
as_storage_record_set_rec_props(as_storage_rd *rd, uint8_t* rec_props_data)
{
	as_rec_props_init(&(rd->rec_props), rec_props_data);

	if (as_index_has_set(rd->r)) {
		const char *set_name = as_index_get_set_name(rd->r, rd->ns);
		as_rec_props_add_field(&(rd->rec_props), CL_REC_PROPS_FIELD_SET_NAME,
				strlen(set_name) + 1, (uint8_t *)set_name);
	}

	if (rd->key) {
		as_rec_props_add_field(&(rd->rec_props), CL_REC_PROPS_FIELD_KEY,
				rd->key_size, rd->key);
	}
}

void
as_storage_shutdown(void)
{
	cf_info(AS_STORAGE, "initiating storage shutdown ...");

	// Pull all record locks - stops everything writing to current swbs such
	// that each write's record lock scope is either completed or never entered.

	for (uint32_t n = 0; n < g_record_locks->n_locks; n++) {
		cf_mutex_lock(&g_record_locks->locks[n]);
	}

	// Now flush everything outstanding to storage devices.

 	cf_info(AS_STORAGE, "flushing data to storage ...");

	for (uint32_t i = 0; i < g_config.n_namespaces; i++) {
		as_namespace *ns = g_config.namespaces[i];

		if (ns->storage_type == AS_STORAGE_ENGINE_SSD) {

			// For now this is only needed for warm-restartable namespaces.
			for (uint32_t pid = 0; pid < AS_PARTITIONS; pid++) {
				as_partition_shutdown(ns, pid);
			}

			as_storage_shutdown_ssd(ns);
			as_namespace_xmem_trusted(ns);
		}
	}

  	cf_info(AS_STORAGE, "completed flushing to storage");
}
