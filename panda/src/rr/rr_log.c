/*
 * Record and Replay for QEMU
 *
 * Copyright (c) 2007-2011 Massachusetts Institute of Technology
 *
 * Authors:
 *   Tim Leek <tleek@ll.mit.edu>
 *   Michael Zhivich <mzhivich@ll.mit.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <libgen.h>

#include <zlib.h>

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qmp-commands.h"
#include "hmp.h"
#include "panda/rr/rr_log.h"
#include "migration/migration.h"
#include "include/exec/address-spaces.h"
#include "migration/qemu-file.h"
#include "io/channel-file.h"
#include "sysemu/sysemu.h"
/******************************************************************************************/
/* GLOBALS */
/******************************************************************************************/
// mz record/replay mode
volatile RR_mode rr_mode = RR_OFF;

// mz FIFO queue of log entries read from the log file
RR_log_entry* rr_queue_head;
RR_log_entry* rr_queue_tail;

// mz 11.06.2009 Flags to manage nested recording
volatile sig_atomic_t rr_record_in_progress = 0;
volatile sig_atomic_t rr_record_in_main_loop_wait = 0;
volatile sig_atomic_t rr_skipped_callsite_location = 0;

// mz the log of non-deterministic events
RR_log* rr_nondet_log = NULL;

#define RR_RECORD_FROM_REQUEST 2
#define RR_RECORD_REQUEST 1

// our own assertion mechanism
#define rr_assert(exp)                                                         \
    if (!(exp)) {                                                              \
        rr_assert_fail(#exp, __FILE__, __LINE__, __FUNCTION__);                \
    }

double rr_get_percentage(void)
{
    return 100.0 * rr_get_guest_instr_count() /
        rr_nondet_log->last_prog_point.guest_instr_count;
}

static inline uint8_t rr_log_is_empty(void)
{
    if ((rr_nondet_log->type == REPLAY) &&
        (rr_nondet_log->size == rr_nondet_log->bytes_read)) {
        return 1;
    } else {
        return 0;
    }
}

RR_debug_level_type rr_debug_level = RR_DEBUG_NOISY;

// used as a signal that TB cache needs flushing.
uint8_t rr_please_flush_tb = 0;

// mz Flags set by monitor to indicate requested record/replay action
volatile sig_atomic_t rr_record_requested = 0;
volatile sig_atomic_t rr_end_record_requested = 0;
volatile sig_atomic_t rr_end_replay_requested = 0;
char* rr_requested_name = NULL;
char* rr_snapshot_name = NULL;

//
// mz Other useful things
//

/******************************************************************************************/
/* UTILITIES */
/******************************************************************************************/

RR_log_entry* rr_get_queue_head(void) { return rr_queue_head; }

// Check if replay is really finished. Conditions:
// 1) The log is empty
// 2) The only thing in the queue is RR_LAST
uint8_t rr_replay_finished(void)
{
    return rr_log_is_empty()
        && rr_queue_head->header.kind == RR_LAST
        && rr_get_guest_instr_count() >=
               rr_queue_head->header.prog_point.guest_instr_count;
}

// mz "performance" counters - basically, how much of the log is taken up by
// mz each kind of entry.
volatile unsigned long long rr_number_of_log_entries[RR_LAST];
volatile unsigned long long rr_size_of_log_entries[RR_LAST];
volatile unsigned long long rr_max_num_queue_entries;

// mz a history of last few log entries for replay
// mz use rr_print_history() to dump in a debugger
#define RR_HIST_SIZE 10
RR_log_entry rr_log_entry_history[RR_HIST_SIZE];
int rr_hist_index = 0;

// write this program point to this file
static void rr_spit_prog_point_fp(RR_prog_point pp)
{
    qemu_log("{guest_instr_count=%llu pc=0x%08llx, secondary=0x%08llx}\n",
             (unsigned long long)pp.guest_instr_count,
             (unsigned long long)pp.pc, (unsigned long long)pp.secondary);
}

void rr_debug_log_prog_point(RR_prog_point pp) { rr_spit_prog_point_fp(pp); }

void rr_spit_prog_point(RR_prog_point pp) { rr_spit_prog_point_fp(pp); }

static void rr_spit_log_entry(RR_log_entry item)
{
    rr_spit_prog_point(item.header.prog_point);
    switch (item.header.kind) {
    case RR_INPUT_1:
        printf("\tRR_INPUT_1 from %s\n",
               get_callsite_string(item.header.callsite_loc));
        break;
    case RR_INPUT_2:
        printf("\tRR_INPUT_2 from %s\n",
               get_callsite_string(item.header.callsite_loc));
        break;
    case RR_INPUT_4:
        printf("\tRR_INPUT_4 from %s\n",
               get_callsite_string(item.header.callsite_loc));
        break;
    case RR_INPUT_8:
        printf("\tRR_INPUT_8 from %s\n",
               get_callsite_string(item.header.callsite_loc));
        break;
    case RR_INTERRUPT_REQUEST:
        printf("\tRR_INTERRUPT_REQUEST from %s\n",
               get_callsite_string(item.header.callsite_loc));
        break;
    case RR_EXIT_REQUEST:
        printf("\tRR_EXIT_REQUEST from %s\n",
               get_callsite_string(item.header.callsite_loc));
        break;
    case RR_SKIPPED_CALL:
        printf("\tRR_SKIPPED_CALL (%s) from %s\n",
               get_skipped_call_kind_string(item.variant.call_args.kind),
               get_callsite_string(item.header.callsite_loc));
        break;
    case RR_LAST:
        printf("\tRR_LAST\n");
        break;
    case RR_DEBUG:
        printf("\tRR_DEBUG\n");
        break;
    default:
        printf("\tUNKNOWN RR log kind %d\n", item.header.kind);
        break;
    }
}

void rr_spit_queue_head(void) { rr_spit_log_entry(*rr_queue_head); }

// mz use in debugger to print a short history of log entries
void rr_print_history(void)
{
    int i = rr_hist_index;
    do {
        rr_spit_log_entry(rr_log_entry_history[i]);
        i = (i + 1) % RR_HIST_SIZE;
    } while (i != rr_hist_index);
}

// mz here to prevent the need to #include<stdio.h> in rr_log.h
void rr_signal_disagreement(RR_prog_point current, RR_prog_point recorded)
{
    printf("FOUND DISAGREEMENT!\n");
    printf("Replay program point:\n");
    rr_spit_prog_point(current);
    printf("\n");
    printf("Record program point:\n");
    rr_spit_prog_point(recorded);
    printf("\n");
    if (current.guest_instr_count != recorded.guest_instr_count) {
        printf(">>> guest instruction counts disagree\n");
    }
}

// our debug rr_assert
static inline void rr_assert_fail(const char* exp, const char* file, int line,
                                  const char* function)
{
    printf("RR rr_assertion `%s' failed at %s:%d\n", exp, file, line);
    printf("Current log point:\n");
    if (rr_queue_head != NULL) {
        rr_spit_prog_point(rr_queue_head->header.prog_point);
        printf("Next log entry type: %s\n",
               log_entry_kind_str[rr_queue_head->header.kind]);
    } else {
        printf("<queue empty>\n");
    }
    printf("Current replay point:\n");
    rr_spit_prog_point(rr_prog_point());
    if (rr_debug_whisper()) {
        qemu_log("RR rr_assertion `%s' failed at %s:%d in %s\n", exp, file,
                 line, function);
    }
    // just abort
    abort();
    rr_end_replay_requested = 1;
    // mz need to get out of cpu loop so that we can process the end_replay
    // request
    // mz this will call cpu_loop_exit(), which longjmps
    // bdg gosh I hope this is OK here. I think it should be as long as we only
    // ever call
    // bdg rr_assert from the CPU loop
    rr_quit_cpu_loop();
    /* NOT REACHED */
}

/******************************************************************************************/
/* RECORD */
/******************************************************************************************/

static inline size_t rr_fwrite(void *ptr, size_t size, size_t nmemb) {
    size_t result = fwrite(ptr, size, nmemb, rr_nondet_log->fp);
    rr_assert(result == nmemb);
    return result;
}

// mz write the current log item to file
static inline void rr_write_item(void)
{
    RR_log_entry item = rr_nondet_log->current_item;

    // mz save the header
    rr_assert(rr_in_record());
    rr_assert(rr_nondet_log != NULL);

#define RR_WRITE_ITEM(field) rr_fwrite(&(field), sizeof(field), 1)
    // mz this is more compact, as it doesn't include extra padding.
    RR_WRITE_ITEM(item.header.prog_point);
    RR_WRITE_ITEM(item.header.kind);
    RR_WRITE_ITEM(item.header.callsite_loc);

    // mz also save the program point in the log structure to ensure that our
    // header will include the latest program point.
    rr_nondet_log->last_prog_point = item.header.prog_point;

    switch (item.header.kind) {
        case RR_INPUT_1:
            RR_WRITE_ITEM(item.variant.input_1);
            break;
        case RR_INPUT_2:
            RR_WRITE_ITEM(item.variant.input_2);
            break;
        case RR_INPUT_4:
            RR_WRITE_ITEM(item.variant.input_4);
            break;
        case RR_INPUT_8:
            RR_WRITE_ITEM(item.variant.input_8);
            break;
        case RR_INTERRUPT_REQUEST:
            RR_WRITE_ITEM(item.variant.interrupt_request);
            break;
        case RR_EXIT_REQUEST:
            RR_WRITE_ITEM(item.variant.exit_request);
            break;
        case RR_SKIPPED_CALL: {
            RR_skipped_call_args* args = &item.variant.call_args;
            // mz write kind first!
            RR_WRITE_ITEM(args->kind);
            switch (args->kind) {
                case RR_CALL_CPU_MEM_RW:
                    RR_WRITE_ITEM(args->variant.cpu_mem_rw_args);
                    rr_fwrite(args->variant.cpu_mem_rw_args.buf, 1,
                            args->variant.cpu_mem_rw_args.len);
                    break;
                case RR_CALL_CPU_MEM_UNMAP:
                    RR_WRITE_ITEM(args->variant.cpu_mem_unmap);
                    rr_fwrite(args->variant.cpu_mem_unmap.buf, 1,
                                args->variant.cpu_mem_unmap.len);
                    break;
                case RR_CALL_MEM_REGION_CHANGE:
                    RR_WRITE_ITEM(args->variant.mem_region_change_args);
                    rr_fwrite(args->variant.mem_region_change_args.name, 1,
                            args->variant.mem_region_change_args.len);
                    break;
                case RR_CALL_HD_TRANSFER:
                    RR_WRITE_ITEM(args->variant.hd_transfer_args);
                    break;
                case RR_CALL_NET_TRANSFER:
                    RR_WRITE_ITEM(args->variant.net_transfer_args);
                    break;
                case RR_CALL_HANDLE_PACKET:
                    RR_WRITE_ITEM(args->variant.handle_packet_args);
                    rr_fwrite(args->variant.handle_packet_args.buf,
                            args->variant.handle_packet_args.size, 1);
                    break;
                default:
                    // mz unimplemented
                    rr_assert(0 && "Unimplemented skipped call!");
            }
        } break;
        case RR_LAST:
        case RR_DEBUG:
            // mz nothing to read
            break;
        default:
            // mz unimplemented
            rr_assert(0 && "Unimplemented replay log entry!");
    }
    rr_nondet_log->item_number++;
}

// bdg in debug mode, to find divergences more quickly
void rr_record_debug(RR_callsite_id call_site)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_DEBUG;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    rr_write_item();
}

// mz record 1-byte CPU input to log file
void rr_record_input_1(RR_callsite_id call_site, uint8_t data)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_INPUT_1;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.input_1 = data;

    rr_write_item();
}

// mz record 2-byte CPU input to file
void rr_record_input_2(RR_callsite_id call_site, uint16_t data)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_INPUT_2;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.input_2 = data;

    rr_write_item();
}

// mz record 4-byte CPU input to file
void rr_record_input_4(RR_callsite_id call_site, uint32_t data)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_INPUT_4;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.input_4 = data;

    rr_write_item();
}

// mz record 8-byte CPU input to file
void rr_record_input_8(RR_callsite_id call_site, uint64_t data)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_INPUT_8;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.input_8 = data;

    rr_write_item();
}

int panda_current_interrupt_request = 0;
/**
 * Save every time cpu->interrupt_request is different than the last time
 * we observed it (panda_current_interrupt_request. In replay, we use these
 * state transitions to always provide the correct value of
 * cpu->interrupt_request without having to record the value every time it is
 * checked
 */
void rr_record_interrupt_request(RR_callsite_id call_site,
                                 uint32_t interrupt_request)
{
    if (panda_current_interrupt_request != interrupt_request) {
        RR_log_entry* item = &(rr_nondet_log->current_item);
        memset(item, 0, sizeof(RR_log_entry));

        item->header.kind = RR_INTERRUPT_REQUEST;
        item->header.callsite_loc = call_site;
        item->header.prog_point = rr_prog_point();

        item->variant.interrupt_request = interrupt_request;
        panda_current_interrupt_request = interrupt_request;
        rr_write_item();
    }
}

void rr_record_exit_request(RR_callsite_id call_site, uint32_t exit_request)
{
    if (exit_request != 0) {
        RR_log_entry* item = &(rr_nondet_log->current_item);
        // mz just in case
        memset(item, 0, sizeof(RR_log_entry));

        item->header.kind = RR_EXIT_REQUEST;
        item->header.callsite_loc = call_site;
        item->header.prog_point = rr_prog_point();

        item->variant.exit_request = exit_request;

        rr_write_item();
    }
}

// mz record call to cpu_physical_memory_rw() that will need to be replayed.
// mz only "write" modifications are recorded
void rr_record_cpu_mem_rw_call(RR_callsite_id call_site, hwaddr addr,
                               const uint8_t* buf, int len, int is_write)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_SKIPPED_CALL;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.call_args.kind = RR_CALL_CPU_MEM_RW;
    item->variant.call_args.variant.cpu_mem_rw_args.addr = addr;
    item->variant.call_args.variant.cpu_mem_rw_args.buf = (uint8_t *)buf;
    item->variant.call_args.variant.cpu_mem_rw_args.len = len;
    // mz is_write is dropped on the floor, as we only record writes

    rr_write_item();
}

// bdg Record the memory modified during a call to
// cpu_physical_memory_map/unmap.
// bdg Really we could subsume the functionality of rr_record_cpu_mem_rw_call
// into this,
// bdg since they're both concerned with capturing the memory side effects of
// device code
void rr_record_cpu_mem_unmap(RR_callsite_id call_site, hwaddr addr,
                             uint8_t* buf, hwaddr len, int is_write)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_SKIPPED_CALL;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.call_args.kind = RR_CALL_CPU_MEM_UNMAP;
    item->variant.call_args.variant.cpu_mem_unmap.addr = addr;
    item->variant.call_args.variant.cpu_mem_unmap.buf = (uint8_t *)buf;
    item->variant.call_args.variant.cpu_mem_unmap.len = len;
    // mz is_write is dropped on the floor, as we only record writes

    rr_write_item();
}

extern QLIST_HEAD(rr_map_list, RR_MapList) rr_map_list;

void rr_tracked_mem_regions_record(void) {
    RR_MapList *region;
    QLIST_FOREACH(region, &rr_map_list, link) {
        uint32_t crc = crc32(0, Z_NULL, 0);
        crc = crc32(crc, region->ptr, region->len);
        if (crc != region->crc) {
            // Pretend this is just a mem_rw call
            rr_device_mem_rw_call_record(region->addr, region->ptr, region->len, 1);
        }
        // Update it so we don't keep recording it
        region->crc = crc;
    }
}

// bdg Record a change in the I/O memory map
void rr_record_memory_region_change(RR_callsite_id call_site,
                                     hwaddr start_addr, uint64_t size,
                                     const char *name, RR_mem_type mtype, bool added)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_SKIPPED_CALL;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.call_args.kind = RR_CALL_MEM_REGION_CHANGE;
    item->variant.call_args.variant.mem_region_change_args.start_addr =
        start_addr;
    item->variant.call_args.variant.mem_region_change_args.size = size;
    item->variant.call_args.variant.mem_region_change_args.name = (char *)name;
    item->variant.call_args.variant.mem_region_change_args.len = strlen(name);
    item->variant.call_args.variant.mem_region_change_args.mtype = mtype;
    item->variant.call_args.variant.mem_region_change_args.added = added;

    rr_write_item();
}

void rr_record_hd_transfer(RR_callsite_id call_site,
                           Hd_transfer_type transfer_type, uint64_t src_addr,
                           uint64_t dest_addr, uint32_t num_bytes)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_SKIPPED_CALL;
    // item->header.qemu_loc = rr_qemu_location;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.call_args.kind = RR_CALL_HD_TRANSFER;
    item->variant.call_args.variant.hd_transfer_args.type = transfer_type;
    item->variant.call_args.variant.hd_transfer_args.src_addr = src_addr;
    item->variant.call_args.variant.hd_transfer_args.dest_addr = dest_addr;
    item->variant.call_args.variant.hd_transfer_args.num_bytes = num_bytes;

    rr_write_item();
}

void rr_record_net_transfer(RR_callsite_id call_site,
                            Net_transfer_type transfer_type, uint64_t src_addr,
                            uint64_t dest_addr, uint32_t num_bytes)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_SKIPPED_CALL;
    // item->header.qemu_loc = rr_qemu_location;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.call_args.kind = RR_CALL_NET_TRANSFER;
    item->variant.call_args.variant.net_transfer_args.type = transfer_type;
    item->variant.call_args.variant.net_transfer_args.src_addr = src_addr;
    item->variant.call_args.variant.net_transfer_args.dest_addr = dest_addr;
    item->variant.call_args.variant.net_transfer_args.num_bytes = num_bytes;

    rr_write_item();
}

void rr_record_handle_packet_call(RR_callsite_id call_site, uint8_t* buf,
                                  int size, uint8_t direction)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_SKIPPED_CALL;
    // item->header.qemu_loc = rr_qemu_location;
    item->header.callsite_loc = call_site;
    item->header.prog_point = rr_prog_point();

    item->variant.call_args.kind = RR_CALL_HANDLE_PACKET;
    item->variant.call_args.variant.handle_packet_args.buf = buf;
    item->variant.call_args.variant.handle_packet_args.size = size;
    item->variant.call_args.variant.handle_packet_args.direction = direction;

    rr_write_item();
}

// mz record a marker for end of the log
static void rr_record_end_of_log(void)
{
    RR_log_entry* item = &(rr_nondet_log->current_item);
    // mz just in case
    memset(item, 0, sizeof(RR_log_entry));

    item->header.kind = RR_LAST;
    item->header.callsite_loc = RR_CALLSITE_LAST;
    item->header.prog_point = rr_prog_point();

    rr_write_item();
}

/******************************************************************************************/
/* REPLAY */
/******************************************************************************************/

// mz avoid actually releasing memory
static RR_log_entry* recycle_list = NULL;

static inline void free_entry_params(RR_log_entry* entry)
{
    // mz cleanup associated resources
    switch (entry->header.kind) {
    case RR_SKIPPED_CALL:
        switch (entry->variant.call_args.kind) {
        case RR_CALL_CPU_MEM_RW:
            g_free(entry->variant.call_args.variant.cpu_mem_rw_args.buf);
            entry->variant.call_args.variant.cpu_mem_rw_args.buf = NULL;
            break;
        case RR_CALL_CPU_MEM_UNMAP:
            g_free(entry->variant.call_args.variant.cpu_mem_unmap.buf);
            entry->variant.call_args.variant.cpu_mem_unmap.buf = NULL;
            break;
        case RR_CALL_HANDLE_PACKET:
            g_free(entry->variant.call_args.variant.handle_packet_args.buf);
            entry->variant.call_args.variant.handle_packet_args.buf = NULL;
            break;
        }
        break;
    case RR_INPUT_1:
    case RR_INPUT_2:
    case RR_INPUT_4:
    case RR_INPUT_8:
    case RR_INTERRUPT_REQUEST:
    default:
        break;
    }
}

// mz "free" a used entry
static inline void add_to_recycle_list(RR_log_entry* entry)
{
    free_entry_params(entry);
    // mz add to the recycle list
    if (recycle_list == NULL) {
        recycle_list = entry;
    } else {
        entry->next = recycle_list;
        recycle_list = entry;
    }
    // mz save item in history
    // mz NB: we're not saving the buffer here (for
    // RR_SKIPPED_CALL/RR_CALL_CPU_MEM_RW),
    // mz so don't try to read it later!
    rr_log_entry_history[rr_hist_index] = *entry;
    rr_hist_index = (rr_hist_index + 1) % RR_HIST_SIZE;
}

// mz allocate a new entry (not filled yet)
static inline RR_log_entry* alloc_new_entry(void)
{
    RR_log_entry* new_entry = NULL;
    if (recycle_list != NULL) {
        new_entry = recycle_list;
        recycle_list = recycle_list->next;
        new_entry->next = NULL;
    } else {
        new_entry = g_new(RR_log_entry, 1);
    }
    memset(new_entry, 0, sizeof(RR_log_entry));
    return new_entry;
}

static inline size_t rr_fread(void *ptr, size_t size, size_t nmemb) {
    size_t result = fread(ptr, size, nmemb, rr_nondet_log->fp);
    rr_nondet_log->bytes_read += nmemb * size;
    rr_assert(result == nmemb);
    return result;
}

// mz fill an entry
static RR_log_entry* rr_read_item(void)
{
    RR_log_entry item;
    item.next = NULL;

    // mz read header
    rr_assert(rr_in_replay());
    rr_assert(!rr_log_is_empty());
    rr_assert(rr_nondet_log->fp != NULL);

    item.header.file_pos = rr_nondet_log->bytes_read;

#define RR_READ_ITEM(field) rr_fread(&(field), sizeof(field), 1)
    // mz this is more compact, as it doesn't include extra padding.
    RR_READ_ITEM(item.header.prog_point);
    RR_READ_ITEM(item.header.kind);
    RR_READ_ITEM(item.header.callsite_loc);

    // mz read the rest of the item
    switch (item.header.kind) {
        case RR_INPUT_1:
            RR_READ_ITEM(item.variant.input_1);
            break;
        case RR_INPUT_2:
            RR_READ_ITEM(item.variant.input_2);
            break;
        case RR_INPUT_4:
            RR_READ_ITEM(item.variant.input_4);
            break;
        case RR_INPUT_8:
            RR_READ_ITEM(item.variant.input_8);
            break;
        case RR_INTERRUPT_REQUEST:
            RR_READ_ITEM(item.variant.interrupt_request);
            break;
        case RR_EXIT_REQUEST:
            RR_READ_ITEM(item.variant.exit_request);
            break;
        case RR_SKIPPED_CALL: {
            RR_skipped_call_args* args = &item.variant.call_args;
            // mz read kind first!
            RR_READ_ITEM(args->kind);
            switch (args->kind) {
                case RR_CALL_CPU_MEM_RW:
                    RR_READ_ITEM(args->variant.cpu_mem_rw_args);
                    // mz buffer length in args->variant.cpu_mem_rw_args.len
                    // mz always allocate a new one. we free it when the item is added
                    // to the recycle list
                    args->variant.cpu_mem_rw_args.buf =
                        g_malloc(args->variant.cpu_mem_rw_args.len);
                    // mz read the buffer
                    rr_fread(args->variant.cpu_mem_rw_args.buf, 1,
                            args->variant.cpu_mem_rw_args.len);
                    break;
                case RR_CALL_CPU_MEM_UNMAP:
                    RR_READ_ITEM(args->variant.cpu_mem_unmap);
                    args->variant.cpu_mem_unmap.buf =
                        g_malloc(args->variant.cpu_mem_unmap.len);
                    rr_fread(args->variant.cpu_mem_unmap.buf, 1,
                                args->variant.cpu_mem_unmap.len);
                    break;
                case RR_CALL_MEM_REGION_CHANGE:
                    RR_READ_ITEM(args->variant.mem_region_change_args);
                    args->variant.mem_region_change_args.name =
                        g_malloc0(args->variant.mem_region_change_args.len + 1);
                    rr_fread(args->variant.mem_region_change_args.name, 1,
                            args->variant.mem_region_change_args.len);
                    break;
                case RR_CALL_HD_TRANSFER:
                    RR_READ_ITEM(args->variant.hd_transfer_args);
                    break;

                case RR_CALL_NET_TRANSFER:
                    RR_READ_ITEM(args->variant.net_transfer_args);
                    break;

                case RR_CALL_HANDLE_PACKET:
                    RR_READ_ITEM(args->variant.handle_packet_args);
                    // mz XXX HACK
                    args->old_buf_addr = (uint64_t)args->variant.handle_packet_args.buf;
                    // mz buffer length in args->variant.cpu_mem_rw_args.len
                    // mz always allocate a new one. we free it when the item is added
                    // to the recycle list
                    args->variant.handle_packet_args.buf =
                        g_malloc(args->variant.handle_packet_args.size);
                    // mz read the buffer
                    rr_fread(args->variant.handle_packet_args.buf,
                            args->variant.handle_packet_args.size, 1);
                    break;

                default:
                    // mz unimplemented
                    rr_assert(0 && "Unimplemented skipped call!");
            }
        } break;
        case RR_LAST:
        case RR_DEBUG:
            // mz nothing to read
            break;
        default:
            // mz unimplemented
            rr_assert(0 && "Unimplemented replay log entry!");
    }
    rr_nondet_log->item_number++;

    // mz let's do some counting
    rr_size_of_log_entries[item.header.kind] +=
        rr_nondet_log->bytes_read - item.header.file_pos;
    rr_number_of_log_entries[item.header.kind]++;

    RR_log_entry *result = alloc_new_entry();
    *result = item;
    return result;
}

#define RR_MAX_QUEUE_LEN 65536

// mz fill the queue of log entries from the file
static void rr_fill_queue(void)
{
    RR_log_entry* log_entry = NULL;
    unsigned long long num_entries = 0;

    // mz first, some sanity checks.  The queue should be empty when this is
    // called.
    rr_assert(rr_queue_head == NULL && rr_queue_tail == NULL);

    while (!rr_log_is_empty()) {
        log_entry = rr_read_item();

        // mz add it to the queue
        if (rr_queue_head == NULL) {
            rr_queue_head = rr_queue_tail = log_entry;
        } else {
            rr_queue_tail->next = log_entry;
            rr_queue_tail = rr_queue_tail->next;
        }
        num_entries++;

        RR_log_entry entry = *log_entry;
        if ((entry.header.kind == RR_SKIPPED_CALL &&
                    entry.header.callsite_loc == RR_CALLSITE_MAIN_LOOP_WAIT)
                || entry.header.kind == RR_INTERRUPT_REQUEST
                || num_entries > RR_MAX_QUEUE_LEN) {
            // Cut off queue so we don't run out of memory on long runs of
            // non-interrupts
            break;
        }
    }
    // mz let's gather some stats
    if (num_entries > rr_max_num_queue_entries) {
        rr_max_num_queue_entries = num_entries;
    }
    static uint64_t next_progress = 1;
    if (rr_get_percentage() >= next_progress) {
        replay_progress();
        next_progress += 1;
    }
}

// mz return next log entry from the queue
static inline RR_log_entry* get_next_entry(RR_log_entry_kind kind,
                                           RR_callsite_id call_site,
                                           bool check_callsite)
{
    RR_log_entry* current;
    // mz make sure queue is not empty, and that we have the right element next
    if (rr_queue_head == NULL) {
        // Try again; we may have failed because the queue got too big and we
        // need to refill
        rr_fill_queue();
        // If it's still empty, fail
        if (rr_queue_head == NULL) {
            printf("Queue is empty, will return NULL\n");
            return NULL;
        }
    }

    if (kind != RR_INTERRUPT_REQUEST && kind != RR_SKIPPED_CALL) {
        while (rr_queue_head && rr_queue_head->header.kind == RR_DEBUG) {
            // printf("Removing RR_DEBUG because we are looking for %s\n",
            // log_entry_kind_str[kind]);
            current = rr_queue_head;
            rr_queue_head = rr_queue_head->next;
            current->next = NULL;
            if (current == rr_queue_tail) {
                rr_queue_tail = NULL;
            }
        }
    }

    RR_log_entry head = *rr_queue_head;
    // XXX FIXME this is a temporary hack to get around the fact that we
    // cannot currently do a tb_flush and a savevm in the same instant.
    if (head.header.prog_point.guest_instr_count == 0) {
        // We'll process this one beacuse it's the start of the log
    }
    // mz rr_prog_point_compare will fail if we're ahead of the log
    else if (rr_prog_point_compare(rr_prog_point(),
                head.header.prog_point, kind) != 0) {
        return NULL;
    }

    if (head.header.kind != kind) {
        return NULL;
    }

    if (check_callsite && head.header.callsite_loc != call_site) {
        return NULL;
    }

    // mz remove log entry from queue and return it.
    current = rr_queue_head;
    rr_queue_head = rr_queue_head->next;
    current->next = NULL;
    if (current == rr_queue_tail) {
        rr_queue_tail = NULL;
    }
    return current;
}

void rr_replay_debug(RR_callsite_id call_site)
{
    RR_log_entry* current_item;

    if (rr_queue_head == NULL) {
        return;
    }

    if (rr_queue_head->header.kind != RR_DEBUG) {
        return;
    }

    RR_prog_point log_point = rr_queue_head->header.prog_point;
    RR_prog_point current = rr_prog_point();

    if (log_point.guest_instr_count > current.guest_instr_count) {
        // This is normal -- in replay we may hit the checkpoint more often
        // than in record due to TB chaining being off
        return;
    } else if (log_point.guest_instr_count == current.guest_instr_count) {
        current_item = rr_queue_head;
        rr_queue_head = rr_queue_head->next;
        current_item->next = NULL;
        if (current_item == rr_queue_tail) {
            rr_queue_tail = NULL;
        }

        add_to_recycle_list(current_item);
        printf("RR_DEBUG check passed: ");
        rr_spit_prog_point(current);
    } else { // log_point.guest_instr_count < current.guest_instr_count
        // This shouldn't happen. We're ahead of the log.
        // rr_signal_disagreement(current, log_point);
        current_item = rr_queue_head;
        rr_queue_head = rr_queue_head->next;
        current_item->next = NULL;
        if (current_item == rr_queue_tail) {
            rr_queue_tail = NULL;
        }

        add_to_recycle_list(current_item);

        // abort();
    }
}

// mz replay 1-byte input to the CPU
void rr_replay_input_1(RR_callsite_id call_site, uint8_t* data)
{
    RR_log_entry* current_item = get_next_entry(RR_INPUT_1, call_site, false);
    if (current_item == NULL) {
        // mz we're trying to replay too early or we have the wrong kind of
        // rr_nondet_log
        // entry.  this is cause for failure
        rr_assert(0);
    }
    // mz now we have our item and it is appropriate for replay here.
    // mz final sanity checks
    rr_assert(current_item->header.callsite_loc == call_site);
    *data = current_item->variant.input_1;
    // mz we've used the item - recycle it.
    add_to_recycle_list(current_item);
}

// mz replay 2-byte input to the CPU
void rr_replay_input_2(RR_callsite_id call_site, uint16_t* data)
{
    RR_log_entry* current_item = get_next_entry(RR_INPUT_2, call_site, false);
    if (current_item == NULL) {
        // mz we're trying to replay too early or we have the wrong kind of
        // rr_nondet_log
        // entry.  this is cause for failure
        rr_assert(0);
    }
    // mz now we have our item and it is appropriate for replay here.
    // mz final sanity checks
    rr_assert(current_item->header.callsite_loc == call_site);
    *data = current_item->variant.input_2;
    // mz we've used the item - recycle it.
    add_to_recycle_list(current_item);
}

// mz replay 4-byte input to the CPU
void rr_replay_input_4(RR_callsite_id call_site, uint32_t* data)
{
    RR_log_entry* current_item = get_next_entry(RR_INPUT_4, call_site, false);

    if (current_item == NULL) {
        // mz we're trying to replay too early or we have the wrong kind of
        // rr_nondet_log
        // entry.  this is cause for failure
        rr_assert(0);
    }

    // mz now we have our item and it is appropriate for replay here.
    // mz final sanity checks
    rr_assert(current_item->header.callsite_loc == call_site);
    *data = current_item->variant.input_4;
    // mz we've used the item - recycle it.
    add_to_recycle_list(current_item);
}

// mz replay 8-byte input to the CPU
void rr_replay_input_8(RR_callsite_id call_site, uint64_t* data)
{
    RR_log_entry* current_item = get_next_entry(RR_INPUT_8, call_site, false);
    if (current_item == NULL) {
        // mz we're trying to replay too early or we have the wrong kind of
        // rr_nondet_log
        // entry.  this is cause for failure
        rr_assert(0);
    }
    // mz now we have our item and it is appropriate for replay here.
    // mz final sanity checks
    rr_assert(current_item->header.callsite_loc == call_site);
    *data = current_item->variant.input_8;
    // mz we've used the item - recycle it.
    add_to_recycle_list(current_item);
}

/**
 * Update the panda_currrent_interrupt_request state machine, if necessary,
 * and use it to return the correct value for cpu->interrupt_requested
 */
void rr_replay_interrupt_request(RR_callsite_id call_site,
                                 uint32_t* interrupt_request)
{
    RR_log_entry* current_item =
        get_next_entry(RR_INTERRUPT_REQUEST, call_site, true);
    if (current_item != NULL) {
        panda_current_interrupt_request = current_item->variant.interrupt_request;
        // mz we've used the item
        add_to_recycle_list(current_item);
        // mz before we can return, we need to fill the queue with information
        // up to the next interrupt value!
        rr_fill_queue();
    }
    *interrupt_request = panda_current_interrupt_request;
}

void rr_replay_exit_request(RR_callsite_id call_site, uint32_t* exit_request)
{
    RR_log_entry* current_item =
        get_next_entry(RR_EXIT_REQUEST, call_site, false);
    if (current_item == NULL) {
        *exit_request = 0;
    } else {
        // mz final sanity checks
        if (current_item->header.callsite_loc != call_site) {
            printf("Callsite match failed; %s (log) != %s (replay)!\n",
                   get_callsite_string(current_item->header.callsite_loc),
                   get_callsite_string(call_site));
            rr_assert(current_item->header.callsite_loc == call_site);
        }
        *exit_request = current_item->variant.exit_request;
        // mz we've used the item
        add_to_recycle_list(current_item);
        // mz before we can return, we need to fill the queue with information
        // up to the next exit_request value!
        // rr_fill_queue();
    }
}

static void rr_create_memory_region(hwaddr start, uint64_t size, RR_mem_type mtype, char *name) {
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    if (mtype == RR_MEM_RAM) {
        Error *err = 0;
        memory_region_init_ram(mr, NULL, name, size, &err);
    } else if (mtype == RR_MEM_IO) {
        memory_region_init_io(mr, NULL, NULL, NULL, name, size);
    }
    memory_region_add_subregion_overlap(get_system_memory(),
            start, mr, 1);
}

static MemoryRegion * rr_memory_region_find_parent(MemoryRegion *root, MemoryRegion *search) {
    MemoryRegion *submr;
    QTAILQ_FOREACH(submr, &root->subregions, subregions_link) {
        if (submr == search) return root;
        MemoryRegion *ssmr = rr_memory_region_find_parent(submr, search);
        if (ssmr) return ssmr;
    }
    return NULL;
}

// mz this function consumes 2 types of entries:
// RR_SKIPPED_CALL_CPU_MEM_RW and RR_SKIPPED_CALL_CPU_REG_MEM_REGION
// XXX call_site parameter no longer used...
// bdg 07.2012: Adding RR_SKIPPED_CALL_CPU_MEM_UNMAP
void rr_replay_skipped_calls_internal(RR_callsite_id call_site)
{
#ifdef CONFIG_SOFTMMU
    uint8_t replay_done = 0;
    do {
        RR_log_entry* current_item =
            get_next_entry(RR_SKIPPED_CALL, call_site, false);
        if (current_item == NULL) {
            // mz queue is empty or we've replayed all we can for this prog
            // point
            replay_done = 1;
        } else {
            RR_skipped_call_args args = current_item->variant.call_args;
            switch (args.kind) {
            case RR_CALL_CPU_MEM_RW: {
                cpu_physical_memory_rw(args.variant.cpu_mem_rw_args.addr,
                                       args.variant.cpu_mem_rw_args.buf,
                                       args.variant.cpu_mem_rw_args.len,
                                       /*is_write=*/1);
            } break;
            case RR_CALL_MEM_REGION_CHANGE: {
                // Add a mapping
                if (args.variant.mem_region_change_args.added) {
                    rr_create_memory_region(
                            args.variant.mem_region_change_args.start_addr,
                            args.variant.mem_region_change_args.size,
                            args.variant.mem_region_change_args.mtype,
                            args.variant.mem_region_change_args.name);
                }
                // Delete a mapping
                else {
                    MemoryRegionSection mrs = memory_region_find(get_system_memory(),
                            args.variant.mem_region_change_args.start_addr,
                            args.variant.mem_region_change_args.size);
                    MemoryRegion *parent = rr_memory_region_find_parent(get_system_memory(),
                            mrs.mr);
                    memory_region_del_subregion(parent, mrs.mr);
                }
            } break;
            case RR_CALL_CPU_MEM_UNMAP: {
                void* host_buf;
                hwaddr plen = args.variant.cpu_mem_unmap.len;
                host_buf = cpu_physical_memory_map(
                    args.variant.cpu_mem_unmap.addr, &plen,
                    /*is_write=*/1);
                memcpy(host_buf, args.variant.cpu_mem_unmap.buf,
                       args.variant.cpu_mem_unmap.len);
                cpu_physical_memory_unmap(host_buf, plen,
                                          /*is_write=*/1,
                                          args.variant.cpu_mem_unmap.len);
            } break;
            default:
                // mz sanity check
                rr_assert(0);
            }
            add_to_recycle_list(current_item);
            // bdg Now that we are also breaking on main loop skipped calls we
            // have to
            // bdg refill the queue here
            // RW ...but only if the queue is actually empty at this point
            if ((call_site == RR_CALLSITE_MAIN_LOOP_WAIT) &&
                (rr_queue_head == NULL)) { // RW queue is empty
                rr_fill_queue();
            }
        }
    } while (!replay_done);
#endif
}

/******************************************************************************************/
/* LOG MANAGEMENT */
/******************************************************************************************/

extern char* qemu_strdup(const char* str);

// create record log
void rr_create_record_log(const char* filename)
{
    // create log
    rr_nondet_log = g_new0(RR_log, 1);
    rr_assert(rr_nondet_log != NULL);

    rr_nondet_log->type = RECORD;
    rr_nondet_log->name = g_strdup(filename);
    rr_nondet_log->fp = fopen(rr_nondet_log->name, "w");
    rr_assert(rr_nondet_log->fp != NULL);

    if (rr_debug_whisper()) {
        qemu_log("opened %s for write.\n", rr_nondet_log->name);
    }
    // mz It would be very handy to know how "far" we are in a particular replay
    // execution.  To do this, let's store a header in the log (we'll fill it in
    // again when we close the log) that includes the maximum instruction
    // count as a monotonicly increasing measure of progress.
    // This way, when we print progress, we can use something better than size
    // of log consumed
    //(as that can jump //sporadically).
    fwrite(&(rr_nondet_log->last_prog_point), sizeof(RR_prog_point), 1,
           rr_nondet_log->fp);
}

// create replay log
void rr_create_replay_log(const char* filename)
{
    struct stat statbuf = {0};
    // create log
    rr_nondet_log = g_new0(RR_log, 1);
    rr_assert(rr_nondet_log != NULL);

    rr_nondet_log->type = REPLAY;
    rr_nondet_log->name = g_strdup(filename);
    rr_nondet_log->fp = fopen(rr_nondet_log->name, "r");
    rr_assert(rr_nondet_log->fp != NULL);

    // mz fill in log size
    stat(rr_nondet_log->name, &statbuf);
    rr_nondet_log->size = statbuf.st_size;
    rr_nondet_log->bytes_read = 0;
    if (rr_debug_whisper()) {
        qemu_log("opened %s for read.  len=%llu bytes.\n", rr_nondet_log->name,
                 rr_nondet_log->size);
    }
    // mz read the last program point from the log header.
    rr_assert(rr_fread(&(rr_nondet_log->last_prog_point),
                sizeof(RR_prog_point), 1) == 1);
}

// close file and free associated memory
void rr_destroy_log(void)
{
    if (rr_nondet_log->fp) {
        // mz if in record, update the header with the last written prog point.
        if (rr_nondet_log->type == RECORD) {
            rewind(rr_nondet_log->fp);
            fwrite(&(rr_nondet_log->last_prog_point), sizeof(RR_prog_point), 1,
                   rr_nondet_log->fp);
        }
        fclose(rr_nondet_log->fp);
        rr_nondet_log->fp = NULL;
    }
    g_free(rr_nondet_log->name);
    g_free(rr_nondet_log);
    rr_nondet_log = NULL;
}

struct timeval replay_start_time;

uint8_t spit_out_total_num_instr_once = 0;

// mz display a measure of replay progress (using instruction counts and log
// size)
void replay_progress(void)
{
    if (rr_nondet_log) {
        if (rr_log_is_empty()) {
            printf("%s:  log is empty.\n", rr_nondet_log->name);
        } else {
            struct rusage rusage;
            getrusage(RUSAGE_SELF, &rusage);

            struct timeval* time = &rusage.ru_utime;
            float secs =
                ((float)time->tv_sec * 1000000 + (float)time->tv_usec) /
                1000000.0;
            char* dup_name = strdup(rr_nondet_log->name);
            char* name = basename(dup_name);
            char* dot = strrchr(name, '.');
            if (dot && dot - name > 10)
                *(dot - 10) = '\0';

            if (!spit_out_total_num_instr_once) {
                spit_out_total_num_instr_once = 1;
                printf("total_instr in replay: %10" PRIu64 "\n",
                       rr_nondet_log->last_prog_point.guest_instr_count);
            }

            printf("%s:  %10" PRIu64
                   " (%6.2f%%) instrs. %7.2f sec. %5.2f GB ram.\n",
                   name, rr_get_guest_instr_count(),
                   ((rr_get_guest_instr_count() * 100.0) /
                    rr_nondet_log->last_prog_point.guest_instr_count),
                   secs, rusage.ru_maxrss / 1024.0 / 1024.0);
            free(dup_name);
        }
    }
}


uint64_t replay_get_total_num_instructions(void)
{
    if (rr_nondet_log) {
        return rr_nondet_log->last_prog_point.guest_instr_count;
    } else {
        return 0;
    }
}

/******************************************************************************************/
/* MONITOR CALLBACKS (top-level) */
/******************************************************************************************/
// mz from vl.c

// rr_name is the current rec/replay name.
// here we compute the snapshot name to use for rec/replay
static inline void rr_get_snapshot_file_name(char* rr_name, char* rr_path,
                                             char* snapshot_name,
                                             size_t snapshot_name_len)
{
    rr_assert(rr_name != NULL);
    snprintf(snapshot_name, snapshot_name_len, "%s/%s-rr-snp", rr_path,
             rr_name);
}

static inline void rr_get_nondet_log_file_name(char* rr_name, char* rr_path,
                                               char* file_name,
                                               size_t file_name_len)
{
    rr_assert(rr_name != NULL && rr_path != NULL);
    snprintf(file_name, file_name_len, "%s/%s-rr-nondet.log", rr_path, rr_name);
}

void rr_reset_state(CPUState* cpu_state)
{
    // set flag to signal that we'll be needing the tb flushed.
    rr_flush_tb_on();
    // clear flags
    rr_record_in_progress = 0;
    rr_skipped_callsite_location = 0;
    cpu_state->rr_guest_instr_count = 0;
}

//////////////////////////////////////////////////////////////
//
// QMP commands

#ifdef CONFIG_SOFTMMU

#include "qapi/error.h"
void qmp_begin_record(const char* file_name, Error** errp)
{
    rr_record_requested = RR_RECORD_REQUEST;
    rr_requested_name = g_strdup(file_name);
}

void qmp_begin_record_from(const char* snapshot, const char* file_name,
                                  Error** errp)
{
    rr_record_requested = RR_RECORD_FROM_REQUEST;
    rr_snapshot_name = g_strdup(snapshot);
    rr_requested_name = g_strdup(file_name);
}

void qmp_end_record(Error** errp)
{
    qmp_stop(NULL);
    rr_end_record_requested = 1;
}

void qmp_end_replay(Error** errp)
{
    qmp_stop(NULL);
    rr_end_replay_requested = 1;
}

void panda_end_replay(void) { rr_end_replay_requested = 1; }

#include "qemu-common.h"    // Monitor def
#include "qapi/qmp/qdict.h" // QDict def

// HMP commands (the "monitor")
void hmp_begin_record(Monitor* mon, const QDict* qdict)
{
    Error* err;
    const char* file_name = qdict_get_try_str(qdict, "file_name");
    qmp_begin_record(file_name, &err);
}

// HMP commands (the "monitor")
void hmp_begin_record_from(Monitor* mon, const QDict* qdict)
{
    Error* err;
    const char* snapshot = qdict_get_try_str(qdict, "snapshot");
    const char* file_name = qdict_get_try_str(qdict, "file_name");
    qmp_begin_record_from(snapshot, file_name, &err);
}

void hmp_end_record(Monitor* mon, const QDict* qdict)
{
    Error* err;
    qmp_end_record(&err);
}

void hmp_end_replay(Monitor* mon, const QDict* qdict)
{
    Error* err;
    qmp_end_replay(&err);
}

#endif // CONFIG_SOFTMMU

static time_t rr_start_time;

// mz file_name_full should be full path to desired record/replay log file
int rr_do_begin_record(const char* file_name_full, CPUState* cpu_state)
{
#ifdef CONFIG_SOFTMMU
    char name_buf[1024];
    // decompose file_name_base into path & file.
    char* rr_path_base = g_strdup(file_name_full);
    char* rr_name_base = g_strdup(file_name_full);
    char* rr_path = dirname(rr_path_base);
    char* rr_name = basename(rr_name_base);
    int snapshot_ret = -1;
    if (rr_debug_whisper()) {
        qemu_log("Begin vm record for file_name_full = %s\n", file_name_full);
        qemu_log("path = [%s]  file_name_base = [%s]\n", rr_path, rr_name);
    }
    // first take a snapshot or load snapshot

    if (rr_record_requested == RR_RECORD_FROM_REQUEST) {
        printf("loading snapshot:\t%s\n", rr_snapshot_name);
        snapshot_ret = load_vmstate(rr_snapshot_name);
        g_free(rr_snapshot_name);
        rr_snapshot_name = NULL;
    }
    if (rr_record_requested == RR_RECORD_REQUEST || rr_record_requested == RR_RECORD_FROM_REQUEST) {
        // Force running state
        global_state_store_running();
        rr_get_snapshot_file_name(rr_name, rr_path, name_buf, sizeof(name_buf));
        printf("writing snapshot:\t%s\n", name_buf);
        QIOChannelFile* ioc =
            qio_channel_file_new_path(name_buf, O_WRONLY | O_CREAT, 0660, NULL);
        QEMUFile* snp = qemu_fopen_channel_output(QIO_CHANNEL(ioc));
        snapshot_ret = qemu_savevm_state(snp, NULL);
        qemu_fclose(snp);
        // log_all_cpu_states();
    }

    // save the time so we can report how long record takes
    time(&rr_start_time);

    // second, open non-deterministic input log for write.
    rr_get_nondet_log_file_name(rr_name, rr_path, name_buf, sizeof(name_buf));
    printf("opening nondet log for write :\t%s\n", name_buf);
    rr_create_record_log(name_buf);
    // reset record/replay counters and flags
    rr_reset_state(cpu_state);
    g_free(rr_path_base);
    g_free(rr_name_base);
    // set global to turn on recording
    rr_mode = RR_RECORD;
    // cpu_set_log(CPU_LOG_TB_IN_ASM|CPU_LOG_RR);
    return snapshot_ret;
#endif
}

void rr_do_end_record(void)
{
#ifdef CONFIG_SOFTMMU
    // mz put in end-of-log marker
    rr_record_end_of_log();

    char* rr_path_base = g_strdup(rr_nondet_log->name);
    char* rr_name_base = g_strdup(rr_nondet_log->name);
    // char *rr_path = dirname(rr_path_base);
    char* rr_name = basename(rr_name_base);

    if (rr_debug_whisper()) {
        qemu_log("End vm record for name = %s\n", rr_name);
        printf("End vm record for name = %s\n", rr_name);
    }

    time_t rr_end_time;
    time(&rr_end_time);
    printf("Time taken was: %ld seconds.\n", rr_end_time - rr_start_time);

    // log_all_cpu_states();

    rr_destroy_log();

    g_free(rr_path_base);
    g_free(rr_name_base);

    // turn off logging
    rr_mode = RR_OFF;
#endif
}

extern void panda_cleanup(void);

// file_name_full should be full path to the record/replay log
int rr_do_begin_replay(const char* file_name_full, CPUState* cpu_state)
{
#ifdef CONFIG_SOFTMMU
    char name_buf[1024];
    // decompose file_name_base into path & file.
    char* rr_path = g_strdup(file_name_full);
    char* rr_name = g_strdup(file_name_full);
    __attribute__((unused)) int snapshot_ret;
    rr_path = dirname(rr_path);
    rr_name = basename(rr_name);
    if (rr_debug_whisper()) {
        qemu_log("Begin vm replay for file_name_full = %s\n", file_name_full);
        qemu_log("path = [%s]  file_name_base = [%s]\n", rr_path, rr_name);
    }
    // first retrieve snapshot
    rr_get_snapshot_file_name(rr_name, rr_path, name_buf, sizeof(name_buf));
    if (rr_debug_whisper()) {
        qemu_log("reading snapshot:\t%s\n", name_buf);
    }
    printf("loading snapshot\n");
    QIOChannelFile* ioc =
        qio_channel_file_new_path(name_buf, O_RDONLY, 0, NULL);
    if (ioc == NULL) {
        printf ("... snapshot file doesn't exist?\n");
        abort();
    }
    QEMUFile* snp = qemu_fopen_channel_input(QIO_CHANNEL(ioc));

    qemu_system_reset(VMRESET_SILENT);
    migration_incoming_state_new(snp);
    snapshot_ret = qemu_loadvm_state(snp);
    qemu_fclose(snp);
    migration_incoming_state_destroy();

    if (snapshot_ret < 0) {
        fprintf(stderr, "Failed to load vmstate\n");
        return snapshot_ret;
    }
    printf("... done.\n");
    // log_all_cpu_states();

    // save the time so we can report how long replay takes
    time(&rr_start_time);

    // second, open non-deterministic input log for read.
    rr_get_nondet_log_file_name(rr_name, rr_path, name_buf, sizeof(name_buf));
    printf("opening nondet log for read :\t%s\n", name_buf);
    rr_create_replay_log(name_buf);
    // reset record/replay counters and flags
    rr_reset_state(cpu_state);
    // set global to turn on replay
    rr_mode = RR_REPLAY;

    // mz fill the queue!
    rr_fill_queue();
    return 0; // snapshot_ret;
#endif
}

// mz XXX what about early replay termination? Can we save state and resume
// later?
void rr_do_end_replay(int is_error)
{
#ifdef CONFIG_SOFTMMU
    // log is empty - we're done
    // dump cpu state at exit as a sanity check.
    int i;
    replay_progress();
    if (is_error) {
        printf("ERROR: replay failed!\n");
    } else {
        printf("Replay completed successfully. 1\n");
    }

    time_t rr_end_time;
    time(&rr_end_time);
    printf("Time taken was: %ld seconds.\n", rr_end_time - rr_start_time);

    printf("Stats:\n");
    for (i = 0; i < RR_LAST; i++) {
        printf("%s number = %llu, size = %llu bytes\n",
               get_log_entry_kind_string(i), rr_number_of_log_entries[i],
               rr_size_of_log_entries[i]);
        rr_number_of_log_entries[i] = 0;
        rr_size_of_log_entries[i] = 0;
    }
    printf("max_queue_len = %llu\n", rr_max_num_queue_entries);
    rr_max_num_queue_entries = 0;
    // cleanup the recycled list for log entries
    {
        unsigned long num_items = 0;
        RR_log_entry* entry;
        while (recycle_list) {
            entry = recycle_list;
            recycle_list = entry->next;
            // mz entry params already freed
            g_free(entry);
            num_items++;
        }
        printf("%lu items on recycle list, %lu bytes total\n", num_items,
               num_items * sizeof(RR_log_entry));
    }
    // mz some more sanity checks - the queue should contain only the RR_LAST
    // element
    if (rr_queue_head == rr_queue_tail && rr_queue_head != NULL &&
        rr_queue_head->header.kind == RR_LAST) {
        printf("Replay completed successfully 2.\n");
    } else {
        if (is_error) {
            printf("ERROR: replay failed!\n");
        } else {
            printf("Replay terminated at user request.\n");
        }
    }
    // cleanup the queue
    {
        RR_log_entry* entry;
        while (rr_queue_head) {
            entry = rr_queue_head;
            rr_queue_head = entry->next;
            entry->next = NULL;
            free_entry_params(entry);
            g_free(entry);
        }
    }
    rr_queue_head = NULL;
    rr_queue_tail = NULL;
    // mz print CPU state at end of replay
    // log_all_cpu_states();
    // close logs
    rr_destroy_log();
    // turn off replay
    rr_mode = RR_OFF;

    // mz XXX something more graceful?
    if (is_error) {
        panda_cleanup();
        abort();
    } else {
        qemu_system_shutdown_request();
    }
#endif // CONFIG_SOFTMMU
}

#ifdef CONFIG_SOFTMMU
uint32_t rr_checksum_memory(void);
uint32_t rr_checksum_memory(void) {
    if (!qemu_in_vcpu_thread()) {
         printf("Need to be in VCPU thread!\n");
         return 0;
    }
    MemoryRegion *ram = memory_region_find(get_system_memory(), 0x2000000, 1).mr;
    rcu_read_lock();
    void *ptr = qemu_map_ram_ptr(ram->ram_block, 0);
    uint32_t crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc, ptr, int128_get64(ram->size));
    rcu_read_unlock();

    return crc;
}

uint32_t rr_checksum_regs(void);
uint32_t rr_checksum_regs(void) {
    if (!qemu_in_vcpu_thread()) {
         printf("Need to be in VCPU thread!\n");
         return 0;
    }
    uint32_t crc = crc32(0, Z_NULL, 0);
    crc = crc32(crc, first_cpu->env_ptr, sizeof(CPUArchState));
    return crc;
}

uint8_t rr_debug_readb(target_ulong addr);
uint8_t rr_debug_readb(target_ulong addr) {
    CPUState *cpu = first_cpu;
    uint8_t out = 0;

    cpu_memory_rw_debug(cpu, addr, (uint8_t *)&out, sizeof(out), 0);
    return out;
}

uint32_t rr_debug_readl(target_ulong addr);
uint32_t rr_debug_readl(target_ulong addr) {
    CPUState *cpu = first_cpu;
    uint32_t out = 0;

    cpu_memory_rw_debug(cpu, addr, (uint8_t *)&out, sizeof(out), 0);
    return out;
}
#endif

/**************************************************************************/
