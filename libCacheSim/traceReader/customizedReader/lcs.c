
#include "lcs.h"

#include <assert.h>
#include <stdio.h>

#include "../customizedReader/binaryUtils.h"
#include "../readerInternal.h"

#ifdef __cplusplus
extern "C" {
#endif

static bool verify(lcs_trace_header_t *header) {
  /* check whether the trace is valid */
  if (header->start_magic != LCS_TRACE_START_MAGIC) {
    ERROR("invalid trace file, start magic is wrong 0x%lx\n", (unsigned long)header->start_magic);
    return false;
  }

  if (header->end_magic != LCS_TRACE_END_MAGIC) {
    ERROR("invalid trace file, end magic is wrong 0x%lx\n", (unsigned long)header->end_magic);
    return false;
  }

  if (header->version > MAX_LCS_VERSION) {
    ERROR("invalid trace file, lcs version %ld is not supported\n", (unsigned long)header->version);
    return false;
  }

  lcs_trace_stat_t *stat = &(header->stat);
  if (stat->n_req < 0 || stat->n_obj < 0) {
    ERROR("invalid trace file, n_req %ld, n_obj %ld\n", (unsigned long)stat->n_req, (unsigned long)stat->n_obj);
    return false;
  }

  return true;
}

int lcsReader_setup(reader_t *reader) {
  char *data = read_bytes(reader, sizeof(lcs_trace_header_t));
  lcs_trace_header_t *header = (lcs_trace_header_t *)data;

  if (!verify(header)) {
    exit(1);
  }

  reader->lcs_ver = header->version;
  reader->trace_type = LCS_TRACE;
  reader->trace_format = BINARY_TRACE_FORMAT;
  reader->trace_start_offset = sizeof(lcs_trace_header_t);
  reader->obj_id_is_num = true;

  if (reader->lcs_ver == 1) {
    reader->item_size = sizeof(lcs_req_v1_t);
  } else if (reader->lcs_ver == 2) {
    reader->item_size = sizeof(lcs_req_v2_t);
  } else if (reader->lcs_ver == 3) {
    reader->item_size = sizeof(lcs_req_v3_t);
  } else {
    ERROR("invalid lcs version %ld\n", (unsigned long)reader->lcs_ver);
    exit(1);
  }

  return 0;
}

// read one request from trace file
// return 0 if success, 1 if error
int lcs_read_one_req(reader_t *reader, request_t *req) {
  char *record = read_bytes(reader, reader->item_size);

  if (record == NULL) {
    req->valid = FALSE;
    return 1;
  }

  if (reader->lcs_ver == 1) {
    lcs_req_v1_t *req_v1 = (lcs_req_v1_t *)record;
    req->clock_time = req_v1->clock_time;
    req->obj_id = req_v1->obj_id;
    req->obj_size = req_v1->obj_size;
    req->next_access_vtime = req_v1->next_access_vtime;
  } else if (reader->lcs_ver == 2) {
    lcs_req_v2_t *req_v2 = (lcs_req_v2_t *)record;
    req->clock_time = req_v2->clock_time;
    req->obj_id = req_v2->obj_id;
    req->obj_size = req_v2->obj_size;
    req->next_access_vtime = req_v2->next_access_vtime;
    req->tenant_id = req_v2->tenant;
    req->op = req_v2->op;
  } else if (reader->lcs_ver == 3) {
    lcs_req_v3_t *req_v3 = (lcs_req_v3_t *)record;
    req->clock_time = req_v3->clock_time;
    req->obj_id = req_v3->obj_id;
    req->obj_size = req_v3->obj_size;
    req->next_access_vtime = req_v3->next_access_vtime;
    req->tenant_id = req_v3->tenant;
    req->op = req_v3->op;
  } else {
    ERROR("invalid lcs version %ld\n", (unsigned long)reader->lcs_ver);
    return 1;
  }

  if (req->next_access_vtime == -1 || req->next_access_vtime == INT64_MAX) {
    req->next_access_vtime = MAX_REUSE_DISTANCE;
  }

  if (req->obj_size == 0 && reader->ignore_size_zero_req && reader->read_direction == READ_FORWARD) {
    return lcs_read_one_req(reader, req);
  }
  return 0;
}

void lcs_print_trace_stat(reader_t *reader) {
  // we need to reset the reader so clone a new one
  reader_t *cloned_reader = clone_reader(reader);
  // reset_reader(cloned_reader);
#ifdef SUPPORT_ZSTD_TRACE
  if (reader->is_zstd_file) {
    fseek(reader->zstd_reader_p->ifile, 0, SEEK_SET);
  }
#endif
  cloned_reader->mmap_offset = 0;

  char *data = read_bytes(cloned_reader, sizeof(lcs_trace_header_t));
  lcs_trace_header_t *header = (lcs_trace_header_t *)data;
  lcs_trace_stat_t *stat = &(header->stat);

  printf("trace stat: n_req %lld, n_obj %lld, n_byte %lld (%.2lf GiB), n_uniq_byte %lld (%.2lf GiB)\n",
         (long long)stat->n_req, (long long)stat->n_obj, (long long)stat->n_req_byte, (double)stat->n_req_byte / GiB,
         (long long)stat->n_obj_byte, (double)stat->n_obj_byte / GiB);

  if (stat->n_read > 0) {
    printf("n_read %lld, n_write %lld, n_delete %lld\n", (long long)stat->n_read, (long long)stat->n_write,
           (long long)stat->n_delete);
  }
  printf("start time %lld, end time %lld, duration %lld seconds %.2lf days\n", (long long)stat->start_timestamp,
         (long long)stat->end_timestamp, (long long)(stat->end_timestamp - stat->start_timestamp),
         (double)(stat->end_timestamp - stat->start_timestamp) / (24 * 3600.0));

  printf("object size: smallest %lld, largest %lld\n", (long long)stat->smallest_obj_size,
         (long long)stat->largest_obj_size);
  printf("most common object sizes (req fraction): %ld(%.4lf) %ld(%.4lf) %ld(%.4lf) %ld(%.4lf)...\n",
         stat->most_common_obj_sizes[0], stat->most_common_obj_size_ratio[0], stat->most_common_obj_sizes[1],
         stat->most_common_obj_size_ratio[1], stat->most_common_obj_sizes[2], stat->most_common_obj_size_ratio[2],
         stat->most_common_obj_sizes[3], stat->most_common_obj_size_ratio[3]);

  printf("highest freq: %ld %ld %ld %ld skewness %.4lf\n", stat->highest_freq[0], stat->highest_freq[1],
         stat->highest_freq[2], stat->highest_freq[3], stat->skewness);
  printf("most common freq (req fraction): %d(%.4lf) %d(%.4lf) %d(%.4lf) %d(%.4lf)...\n", stat->most_common_freq[0],
         stat->most_common_freq_ratio[0], stat->most_common_freq[1], stat->most_common_freq_ratio[1],
         stat->most_common_freq[2], stat->most_common_freq_ratio[2], stat->most_common_freq[3],
         stat->most_common_freq_ratio[3]);

  if (stat->n_tenant > 1) {
    printf("#tenant: %ld\n", (long)stat->n_tenant);
    printf("most common tenants (req fraction): %d(%.4lf) %d(%.4lf) %d(%.4lf) %d(%.4lf)...\n",
           stat->most_common_tenants[0], stat->most_common_tenant_ratio[0], stat->most_common_tenants[1],
           stat->most_common_tenant_ratio[1], stat->most_common_tenants[2], stat->most_common_tenant_ratio[2],
           stat->most_common_tenants[3], stat->most_common_tenant_ratio[3]);
  }

  if (stat->n_ttl > 1) {
    printf("#ttl: %ld\n", (long)stat->n_ttl);
    printf("smallest ttl: %ld, largest ttl: %ld\n", (long)stat->smallest_ttl, (long)stat->largest_ttl);
    printf("most common ttls (req fraction): %d(%.4lf) %d(%.4lf) %d(%.4lf) %d(%.4lf)...\n", stat->most_common_ttls[0],
           stat->most_common_ttl_ratio[0], stat->most_common_ttls[1], stat->most_common_ttl_ratio[1],
           stat->most_common_ttls[2], stat->most_common_ttl_ratio[2], stat->most_common_ttls[3],
           stat->most_common_ttl_ratio[3]);
  }

  close_reader(cloned_reader);
}

#ifdef __cplusplus
}
#endif