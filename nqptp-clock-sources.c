/*
 * This file is part of the nqptp distribution (https://github.com/mikebrady/nqptp).
 * Copyright (c) 2021 Mike Brady.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial licensing is also available.
 */

#include "nqptp-clock-sources.h"
#include "debug.h"
#include "general-utilities.h"
#include "nqptp-ptp-definitions.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <string.h>
#include <sys/types.h>

#ifdef CONFIG_FOR_FREEBSD
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#ifndef FIELD_SIZEOF
#define FIELD_SIZEOF(t, f) (sizeof(((t *)0)->f))
#endif

clock_source_private_data clocks_private[MAX_CLOCKS];

int find_clock_source_record(char *sender_string, clock_source_private_data *clocks_private_info) {
  // return the index of the clock in the clock information arrays or -1
  int response = -1;
  int i = 0;
  int found = 0;
  while ((found == 0) && (i < MAX_CLOCKS)) {
    if ((clocks_private_info[i].in_use != 0) &&
        (strcasecmp(sender_string, (const char *)&clocks_private_info[i].ip) == 0))
      found = 1;
    else
      i++;
  }
  if (found == 1)
    response = i;
  return response;
}

int create_clock_source_record(char *sender_string,
                               clock_source_private_data *clocks_private_info) {
  // sometimes, the mutex will already be locked
  // return the index of a clock entry in the clock information arrays or -1 if full
  // initialise the entries in the shared and private arrays
  int response = -1;
  int i = 0;
  int found = 0;
  while ((found == 0) && (i < MAX_CLOCKS)) {
    if (clocks_private_info[i].in_use == 0)
      found = 1;
    else
      i++;
  }

  if (found == 1) {
    response = i;
    memset(&clocks_private_info[i], 0, sizeof(clock_source_private_data));
    strncpy((char *)&clocks_private_info[i].ip, sender_string,
            FIELD_SIZEOF(clock_source_private_data, ip) - 1);
    clocks_private_info[i].vacant_samples = MAX_TIMING_SAMPLES;
    clocks_private_info[i].in_use = 1;
    debug(2, "create record for ip: %s.", &clocks_private_info[i].ip);
  } else {
    die("Clock tables full!");
  }
  return response;
}

void manage_clock_sources(uint64_t reception_time, clock_source_private_data *clocks_private_info) {
  debug(3, "manage_clock_sources");
  int i;

  // do a garbage collect for clock records no longer in use
  for (i = 0; i < MAX_CLOCKS; i++) {
    // only if its in use and not a timing peer... don't need a mutex to check
    if ((clocks_private_info[i].in_use != 0) &&
        ((clocks_private_info[i].flags & (1 << clock_is_a_timing_peer)) == 0)) {
      int64_t time_since_last_use = reception_time - clocks_private_info[i].time_of_last_use;
      // using a sync timeout to determine when to drop the record...
      // the following give the sync receipt time in whole seconds
      // depending on the aPTPinitialLogSyncInterval and the aPTPsyncReceiptTimeout
      int64_t syncTimeout = (1 << (32 + aPTPinitialLogSyncInterval));
      syncTimeout = syncTimeout * aPTPsyncReceiptTimeout;
      syncTimeout = syncTimeout >> 32;
      // seconds to nanoseconds
      syncTimeout = syncTimeout * 1000000000;
      if (time_since_last_use > syncTimeout) {
        uint32_t old_flags = clocks_private_info[i].flags;
        debug(2, "delete record for: %s.", &clocks_private_info[i].ip);
        memset(&clocks_private_info[i], 0, sizeof(clock_source_private_data));
        if (old_flags != 0)
          update_master();
      }
    }
  }
}

// check all the entries in the clock array and mark all those that
// belong to ourselves

void update_clock_self_identifications(clock_source_private_data *clocks_private_info) {
  // first, turn off all the self-id flags
  int i;
  for (i = 0; i < MAX_CLOCKS; i++) {
    clocks_private_info[i].is_one_of_ours = 0;
  }

  struct ifaddrs *ifap, *ifa;
  void *addr = NULL;
  short family;
  getifaddrs(&ifap);
  for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
    family = ifa->ifa_addr->sa_family;
#ifdef AF_INET6
    if (ifa->ifa_addr && family == AF_INET6) {
      struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)ifa->ifa_addr;
      addr = &(sa6->sin6_addr);
    }
#endif
    if (ifa->ifa_addr && family == AF_INET) {
      struct sockaddr_in *sa4 = (struct sockaddr_in *)ifa->ifa_addr;
      addr = &(sa4->sin_addr);
    }
    char ip_string[64];
    memset(ip_string, 0, sizeof(ip_string));
    if (addr != NULL)
      inet_ntop(family, addr, ip_string, sizeof(ip_string));
    if (strlen(ip_string) != 0) {
      // now set the is_one_of_ours flag of any clock with this ip
      for (i = 0; i < MAX_CLOCKS; i++) {
        if (strcasecmp(ip_string, clocks_private_info[i].ip) == 0) {
          debug(2, "found an entry for one of our clocks");
          clocks_private_info[i].is_one_of_ours = 1;
        }
      }
    }
  }
  freeifaddrs(ifap);
}

void update_master() {
  // note -- this is definitely incomplete -- it doesn't do the full
  // data set comparison specified by the IEEE 588 standard
  int old_master = -1;
  // find the current master clock if there is one and turn off all mastership
  int i;
  for (i = 0; i < MAX_CLOCKS; i++) {
    if ((clocks_private[i].flags & (1 << clock_is_master)) != 0)
      if (old_master == -1)
        old_master = i;                                 // find old master
    clocks_private[i].flags &= ~(1 << clock_is_master); // turn them all off
  }

  int best_so_far = -1;
  int timing_peer_count = 0;
  uint32_t acceptance_mask =
      (1 << clock_is_qualified) | (1 << clock_is_a_timing_peer) | (1 << clock_is_valid);
  for (i = 0; i < MAX_CLOCKS; i++) {
    if ((clocks_private[i].flags & acceptance_mask) == acceptance_mask) {
      // found a possible clock candidate
      timing_peer_count++;
      if (best_so_far == -1) {
        best_so_far = i;
      } else {
        // do the data set comparison detailed in Figure 27 and Figure 28 on pp89-90
        if (clocks_private[i].grandmasterIdentity ==
            clocks_private[best_so_far].grandmasterIdentity) {
          // should implement Figure 28 here
        } else if (clocks_private[i].grandmasterPriority1 <
                   clocks_private[best_so_far].grandmasterPriority1) {
          best_so_far = i;
        } else if (clocks_private[i].grandmasterClass <
                   clocks_private[best_so_far].grandmasterClass) {
          best_so_far = i;
        } else if (clocks_private[i].grandmasterAccuracy <
                   clocks_private[best_so_far].grandmasterAccuracy) {
          best_so_far = i;
        } else if (clocks_private[i].grandmasterVariance <
                   clocks_private[best_so_far].grandmasterVariance) {
          best_so_far = i;
        } else if (clocks_private[i].grandmasterPriority2 <
                   clocks_private[best_so_far].grandmasterPriority2) {
          best_so_far = i;
        } else if (clocks_private[i].grandmasterIdentity <
                   clocks_private[best_so_far].grandmasterIdentity) {
          best_so_far = i;
        }
      }
    }
  }
  if (best_so_far == -1) {
    // no master clock
    if (old_master != -1) {
      // but there was a master clock, so remove it
      debug(1, "shm interface -- remove master clock designation");
      update_master_clock_info(0, NULL, 0, 0, 0);
    }
    if (timing_peer_count == 0)
      debug(2, "No timing peer list found");
    else
      debug(1, "No master clock not found!");
  } else {
    // we found a master clock
    clocks_private[best_so_far].flags |= (1 << clock_is_master);

    if (old_master != best_so_far) {
      uint64_t oldest_acceptable_master_clock_time =
          clocks_private[best_so_far].source_time + 1150000000; // ns.

      // we will try to improve on this present, definitive, local_to_source_time_offset we have
      int changes_made = 0;

      uint64_t best_offset_so_far = clocks_private[best_so_far].local_to_source_time_offset;
      uint64_t age_of_oldest_legitimate_sample = clocks_private[best_so_far].local_time;

      int number_of_samples = MAX_TIMING_SAMPLES - clocks_private[best_so_far].vacant_samples;
      int samples_checked = 0;
      if (number_of_samples > 0) {
        debug(3, "Number of samples: %d.", number_of_samples);
        uint64_t time_now = get_time_now();

        // Now we use the last few samples to calculate the best offset for the
        // new master clock.

        // The time of the oldest sample we use will become the time of the start of the
        // mastership

        // We will accept samples that would make the local-to-clock offset greatest,
        // provided they are not too old and that they don't push the current clock time
        // more than, say, 1000 ms plus one sample interval (i.e about 1.125 seconds) in the future.

        // This present sample is the only time estimate we have when the clock is definitely a
        // master, so we use it to eliminate any previous time estimates, made when the clock wasn't
        // designated a master, that would put it more than, say, a 1.15 seconds further into the
        // future.

        // Allow the samples to give a valid master clock time up to this much later than the
        // present, definitive, sample:

        uint64_t oldest_acceptable_time = time_now - 10000000000; // only go back this far (ns)

        int64_t cko = age_of_oldest_legitimate_sample - oldest_acceptable_time;
        if (cko < 0)
          debug(1,"starting sample is too old: %" PRId64 " ns.", cko);

        int i;
        for (i = 0; i < number_of_samples; i++) {

          int64_t age = time_now - clocks_private[best_so_far].samples[i].local_time;

          int64_t age_relative_to_oldest_acceptable_time =
              clocks_private[best_so_far].samples[i].local_time - oldest_acceptable_time;
          if (age_relative_to_oldest_acceptable_time > 0) {
            debug(3, "sample accepted at %f seconds old.", 0.000000001 * age);
            if (clocks_private[best_so_far].samples[i].local_time <
                age_of_oldest_legitimate_sample) {
              age_of_oldest_legitimate_sample = clocks_private[best_so_far].samples[i].local_time;
            }
            uint64_t possible_offset = clocks_private[best_so_far].samples[i].clock_time -
                                       clocks_private[best_so_far].samples[i].local_time;
            uint64_t possible_master_clock_time =
                clocks_private[best_so_far].local_time + possible_offset;
            int64_t age_relative_to_oldest_acceptable_master_clock_time =
                possible_master_clock_time - oldest_acceptable_master_clock_time;
            if (age_relative_to_oldest_acceptable_master_clock_time <= 0) {
              samples_checked++;
              // so, the sample was not obtained too far in the past
              // and it would not push the estimated master clock_time too far into the future
              // so, if it is greater than the best_offset_so_far, then make it the new one
              if (possible_offset > best_offset_so_far) {
                debug(3, "new best offset");
                best_offset_so_far = possible_offset;
                changes_made++;
              }
            } else {
              debug(3, "sample too far into the future");
            }
          } else {
            debug(3, "sample too old at %f seconds old.", 0.000000001 * age);
          }
        }
      }

      // it is possible that the clock has been designated master without any valid recent samples
      // in which case the number of valid samples will be zero.
      if (samples_checked == 0) {
        debug(2,"clock %" PRIx64 " has become master without any recent samples...", clocks_private[best_so_far].clock_id);
        // having no samples is a flag for the first FOLLOW_UP to set the bus mastership start time
        // to its own reception time.
        clocks_private[best_so_far].vacant_samples = MAX_TIMING_SAMPLES; // discard all samples
      } else {
        clocks_private[best_so_far].mastership_start_time = age_of_oldest_legitimate_sample;
        int64_t offset_difference =
            best_offset_so_far - clocks_private[best_so_far].local_to_source_time_offset;

        debug(2, "Lookback difference: %f ms with %d samples checked of %d samples total.",
              0.000001 * offset_difference, samples_checked, number_of_samples);
        clocks_private[best_so_far].local_to_source_time_offset = best_offset_so_far;

        debug(2, "Master sampling started %f ms before becoming master.",
              0.000001 * (clocks_private[best_so_far].local_time - age_of_oldest_legitimate_sample));
        update_master_clock_info(clocks_private[best_so_far].clock_id,
                                 (const char *)&clocks_private[best_so_far].ip,
                                 clocks_private[best_so_far].local_time,
                                 clocks_private[best_so_far].local_to_source_time_offset,
                                 clocks_private[best_so_far].mastership_start_time);
      }
      clocks_private[best_so_far].previous_offset_time = 0; // resync
    }
  }

  int records_in_use = 0;
  for (i = 0; i < MAX_CLOCKS; i++)
    if (clocks_private[i].in_use != 0)
      records_in_use++;
  if (records_in_use > 0) {
    debug(1, "");
    debug(1, "Current NQPTP Status:");
    uint32_t peer_mask = (1 << clock_is_a_timing_peer);
    uint32_t peer_clock_mask = peer_mask | (1 << clock_is_valid);
    uint32_t peer_master_mask = peer_clock_mask | (1 << clock_is_master);
    uint32_t non_peer_clock_mask = (1 << clock_is_valid);
    uint32_t non_peer_master_mask = non_peer_clock_mask | (1 << clock_is_master);
    for (i = 0; i < MAX_CLOCKS; i++) {
      if (clocks_private[i].in_use != 0) {
        if ((clocks_private[i].flags & peer_master_mask) == peer_master_mask) {
          debug(1, "  Peer Master:     %" PRIx64 "  %s.", clocks_private[i].clock_id,
                clocks_private[i].ip);
        } else if ((clocks_private[i].flags & peer_clock_mask) == peer_clock_mask) {
          debug(1, "  Peer Clock:      %" PRIx64 "  %s.", clocks_private[i].clock_id,
                clocks_private[i].ip);
        } else if ((clocks_private[i].flags & peer_mask) == peer_mask) {
          debug(1, "  Peer:                              %s.", clocks_private[i].ip);
        } else if ((clocks_private[i].flags & non_peer_master_mask) == non_peer_master_mask) {
          debug(1, "  Non Peer Master: %" PRIx64 "  %s.", clocks_private[i].clock_id,
                clocks_private[i].ip);
        } else if ((clocks_private[i].flags & non_peer_clock_mask) == non_peer_clock_mask) {
          debug(1, "  Non Peer Clock:  %" PRIx64 "  %s.", clocks_private[i].clock_id,
                clocks_private[i].ip);
        } else {
          debug(1, "  Non Peer Record:                   %s.", clocks_private[i].ip);
        }
      }
    }
  }
}
