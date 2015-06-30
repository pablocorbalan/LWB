/*
 * @remarks:
 * - TIME_SCALE removed (i.e. fixed to 1)
 * - dynamic period only
 * - JOINING_NODES, TWO_SCHEDS, COMPRESS, REMOVE_NODES and DYNAMIC_FREE_SLOTS set to 1 (i.e. removed)
 * - everything with MINIMIZE_LATENCY removed
 * - external memory support added
 * - list for pending S-ACKs added
 */
 
#include "s-lwb.h"


uint16_t sched_compress(uint8_t* compressed_data, uint8_t n_slots);

static uint8_t        period;
static uint32_t       time;                                 // global time
static uint16_t       n_streams;                            // number of streams
static sched_stats_t  sched_stats = { 0 };
static uint8_t        first_index;                          // offset for the stream list
static uint8_t        n_slots_assigned;                        // number of slots assigned in the schedule
static uint32_t       data_cnt;
static uint16_t       data_ipi;
static volatile uint8_t n_pending_sack = 0;
static uint8_t        pending_sack[4 * N_PENDING_SACK_MAX]; // note: 4 * ... because of the memory alignment and faster index calculation!
#ifndef SCHEDULER_USE_XMEM
  static stream_info_t  *streams[N_SLOTS_MAX];              // a list of pointers to the stream info structures, for faster access (constant time vs. linear time)
  LIST(streams_list);                                       // -> lists only work for data in RAM
  MEMB(streams_memb, stream_info_t, N_STREAMS_MAX);         // data structures to hold the stream info
#else
  static uint32_t     streams_list = MEMBX_INVALID_ADDR;     // address of the first linked list element (head) in the external memory (note: do NOT dereference this pointer!)
  MEMBX(streams_memb, sizeof(stream_info_t), N_STREAMS_MAX);// data structures to hold the stream info, starting at address 0 in the external memory
#endif // SCHEDULER_USE_XMEM



#ifndef SCHEDULER_USE_XMEM
/**
 * @brief   remove a stream from the stream list on the host
 * @param[in] the stream to remove
 */
static inline void sched_del_stream(stream_info_t* stream) {
    if (stream == 0) {        
        return;    // entry not found, don't do anything
    }
    uint16_t node_id    = stream->node_id;
    uint8_t  stream_id  = stream->stream_id;
    list_remove(streams_list, stream);
    memb_free(&streams_memb, stream);
    n_streams--;
    sched_stats.n_deleted++;
    
    DEBUG_PRINT_INFO("stream %u.%u removed", node_id, stream_id);
}
#else
/**
 * @brief   remove a stream from the stream list on the host
 * @param[in] address (in the ext. mem.) of the stream to remove
 */
void sched_del_stream(uint32_t stream_addr) {
    if (stream_addr == MEMBX_INVALID_ADDR || streams_list == MEMBX_INVALID_ADDR) {        
        return;
    }    
    stream_info_t stream;
    uint16_t      node_id;
    uint8_t       stream_id;
    fram_read(stream_addr, sizeof(stream_info_t), (uint8_t*)&stream);
    uint32_t next_addr = stream.next; 
    if (streams_list == stream_addr) {       // special case: it's the first element
        node_id    = stream.node_id;
        stream_id  = stream.stream_id;
        streams_list = next_addr; 
    } else {
        uint32_t prev_addr = streams_list;
        do {
            fram_read(prev_addr, sizeof(stream_info_t), (uint8_t*)&stream);
            if (stream.next == stream_addr) {
                node_id    = stream.node_id;
                stream_id  = stream.stream_id;
                break;
            }
            prev_addr = stream.next;
        } while (prev_addr != MEMBX_INVALID_ADDR);
        if (prev_addr == MEMBX_INVALID_ADDR) {
            // not found!
            DEBUG_PRINT_WARNING("memory block not freed (invalid address)");
            return;
        }
        fram_read(prev_addr, sizeof(stream_info_t), (uint8_t*)&stream);   // read
        stream.next = next_addr;                                // adjust the next-pointer
        fram_write(prev_addr, sizeof(stream_info_t), (uint8_t*)&stream);  // write back
    }
    membx_free(&streams_memb, stream_addr);  // mark the memory block as free
    n_streams--;
    sched_stats.n_deleted++;
    
    DEBUG_PRINT_INFO("stream %u.%u removed", node_id, stream_id);
}
#endif // SCHEDULER_USE_XMEM


/**
 * @brief   prepare a stream acknowledgement (S-ACK) packet
 * @param[out] payload output buffer
 * @return  the packet size or zero if there is no S-ACK pending
 */
uint8_t sched_prep_sack(glossy_payload_t *payload) {
    if (n_pending_sack) {
        DEBUG_PRINT_VERBOSE("%u S-ACKs pending", n_pending_sack);
        memcpy(&payload->sack_pkt, pending_sack, n_pending_sack * 4);
        payload->sack_pkt.n_extra = n_pending_sack - 1;
        n_pending_sack = 0;
        return (payload->sack_pkt.n_extra + 1) * 4;
    }
    return 0;               // return the length of the packet
}


/**
 * @brief  processes a stream request
 * adds new streams to the stream list, updates stream information for existing streams or removes streams with an invalid IPI
 * @param[in] req the stream request to process
 */
void sched_proc_srq(const stream_request_t * req) {
#ifndef SCHEDULER_USE_XMEM
    stream_info_t *stream  = 0;     
#else
    stream_info_t stream, new_stream;
    uint32_t stream_addr, prev_addr;
#endif // SCHEDULER_USE_XMEM
    sched_stats.t_last_req = time;
       
    if (n_pending_sack >= N_PENDING_SACK_MAX) {
        DEBUG_PRINT_WARNING("max. number of pending sack's reached, stream request dropped");
        return;
    }
    
    if (req->ipi > 0) { // add and remove requests are implicitly given by the ipi (i.e. ipi of 0 implies 'remove')
    
#ifndef SCHEDULER_USE_XMEM
        // check if stream already exists
        if (n_streams) {
            for (stream = list_head(streams_list); stream != 0; stream = stream->next) {
                if (req->node_id == stream->node_id && req->stream_id == stream->stream_id) {
                    // already exists -> update the IPI
                    stream->ipi = req->ipi;
                    stream->last_assigned = time + req->t_offset;
                    // insert into the list of pending S-ACKs
                    memcpy(pending_sack + n_pending_sack * 4, &req->node_id, 2);    // must do it this way or make pending_sack of type uint16_t (due to pointer alignment)
                    pending_sack[n_pending_sack * 4 + 2] = req->stream_id;
                    n_pending_sack++;
                    DEBUG_PRINT_VERBOSE("stream request %u.%u processed (IPI updated)", req->node_id, req->stream_id);
                    return;
                }
            }  
        }
        // does not exist: add the new stream
        stream = memb_alloc(&streams_memb);
        if (stream == 0) {
            DEBUG_PRINT_WARNING("out of memory: stream request dropped");
            sched_stats.n_no_space++;    // no space for new streams
            return;
        }
        stream->node_id         = req->node_id;
        stream->ipi             = req->ipi;
        stream->last_assigned   = ((int32_t)time + (int32_t)req->t_offset > 0) ? (time + req->t_offset) : time;     //(time > req->ipi) ? (time - req->ipi) : time;
        stream->stream_id       = req->stream_id;
        stream->n_cons_missed   = 0;
        // insert the stream into the list, ordered by node id
        stream_info_t *prev_stream;
        for (prev_stream = list_head(streams_list); prev_stream != NULL; prev_stream = prev_stream->next) {
            if ((req->node_id >= prev_stream->node_id) && ((prev_stream->next == NULL) || (req->node_id < prev_stream->next->node_id))) {
                break;
            }
        }
        list_insert(streams_list, prev_stream, stream);   
#else          
        // check if stream already exists
        if (n_streams) {
            stream_addr = streams_list;
            do {
                // load the first block
                fram_read(stream_addr, sizeof(stream_info_t), (uint8_t*)&stream);
                // check the ID
                if (req->node_id == stream.node_id && req->stream_id == stream.stream_id) {
                    // already exists -> update the IPI
                    stream.ipi = req->ipi;
                    stream.last_assigned = time + req->t_offset;  
                    // insert into the list of pending S-ACKs                  
                    memcpy(pending_sack + n_pending_sack * 4, &req->node_id, 2);    // must do it this way or make pending_sack of type uint16_t (due to pointer alignment)
                    pending_sack[n_pending_sack * 4 + 2] = req->stream_id;
                    n_pending_sack++;
                    DEBUG_PRINT_VERBOSE("stream request %u.%u processed (IPI updated)", req->node_id, req->stream_id);
                    // save the changes
                    fram_write(stream_addr, sizeof(stream_info_t), (uint8_t*)&stream);
                    return;
                }
                // go to the next address
                stream_addr = stream.next;            
            } while (stream_addr != MEMBX_INVALID_ADDR);
        }                
        // does not exist: add the new stream
        stream_addr = membx_alloc(&streams_memb);
        if (stream_addr == MEMBX_INVALID_ADDR) {
            DEBUG_PRINT_WARNING("no memory available to store stream info");
            sched_stats.n_no_space++;    // no space for new streams
            return;
        }
        new_stream.node_id         = req->node_id;
        new_stream.ipi             = req->ipi;
        new_stream.last_assigned   = ((int32_t)time + (int32_t)req->t_offset > 0) ? (time + req->t_offset) : time;     //(time > req->ipi) ? (time - req->ipi) : time;
        new_stream.stream_id       = req->stream_id;
        new_stream.n_cons_missed   = 0;
        new_stream.next            = MEMBX_INVALID_ADDR;
        // insert the stream into the list, ordered by node id
        if (streams_list == MEMBX_INVALID_ADDR) {   // empty list?
            streams_list = stream_addr;
        } else {
            prev_addr = streams_list;
            do {
                fram_read(prev_addr, sizeof(stream_info_t), (uint8_t*)&stream);
                // check the ID
                if (req->node_id <= stream.node_id || stream.next == MEMBX_INVALID_ADDR) { 
                    if (streams_list == prev_addr) { // the element is inserted at the head of the list
                        streams_list = stream_addr;
                        new_stream.next = prev_addr;
                    } else {
                        new_stream.next = stream.next;
                        stream.next = stream_addr;
                        fram_write(prev_addr, sizeof(stream_info_t), (uint8_t*)&stream);
                    }                
                    break;
                }            
                prev_addr = stream.next;    // go to the next address
            } while (prev_addr != MEMBX_INVALID_ADDR);            
        }
        fram_write(stream_addr, sizeof(stream_info_t), (uint8_t*)&new_stream);

#endif  // SCHEDULER_USE_XMEM
  
        n_streams++;
        sched_stats.n_added++; 
        
        DEBUG_PRINT_VERBOSE("stream request from node %u processed (stream %u added)", req->node_id, req->stream_id);
               
    } else {
#ifndef SCHEDULER_USE_XMEM    
        // remove this stream
        for (stream = list_head(streams_list); stream != 0; stream = stream->next) {
            if (req->node_id == stream->node_id) {
                break;
            }
        }
        sched_del_stream(stream);        
#else
        stream_addr = streams_list;
        while (stream_addr != MEMBX_INVALID_ADDR) {
            fram_read(stream_addr, sizeof(stream_info_t), (uint8_t*)&stream);
            if (req->node_id == stream.node_id) {
                break;
            }
            stream_addr = stream.next;  // go to the next address   
        }
        sched_del_stream(stream_addr);
#endif  // SCHEDULER_USE_XMEM        
    }
            
    // insert into the list of pending S-ACKs
    memcpy(pending_sack + n_pending_sack * 4, &req->node_id, 2);    // must do it this way or make pending_sack of type uint16_t (due to pointer alignment)
    pending_sack[n_pending_sack * 4 + 2] = req->stream_id;
    n_pending_sack++;   
}


/**
 * @brief implementation of the binary GCD algorithm
 * @param[in] u an unsigned integer
 * @param[in] v an unsigned integer
 * @return the greatest common divider of u and v
 */
static inline uint16_t gcd(uint16_t u, uint16_t v) {
    uint16_t shift;

    /* GCD(0,x) := x */
    if (u == 0 || v == 0) {
      return u | v;
    }

    /* Let shift := lg K, where K is the greatest power of 2
       dividing both u and v. */
    for (shift = 0; ((u | v) & 1) == 0; ++shift) {
        u >>= 1;
        v >>= 1;
    }
    while ((u & 1) == 0) {
        u >>= 1;
    }
    /* From here on, u is always odd. */
    do {
        while ((v & 1) == 0) {  /* Loop X */
            v >>= 1;
        }
        /* Now u and v are both odd, so diff(u, v) is even.
           Let u = min(u, v), v = diff(u, v)/2. */
        if (u < v) {
            v -= u;
        } else {
            uint16_t diff = u - v;
            u = v;
            v = diff;
        }
        v >>= 1;
    } while (v != 0);

    return u << shift;
}


/**
 * @brief adapts the communication period T according to the traffic demand
 * @return the new period
 */
static inline uint16_t sched_adapt_period(void) {    
    if (time < (sched_stats.t_last_req + T_NO_REQ)) {
        // we have received stream requests in the last T_NO_REQ seconds: set the period to a low value
        return PERIOD_MIN;
    }
    if (!data_cnt) {
        // no streams
        return PERIOD_IDLE; 
    }
    uint16_t new_period = (uint16_t)(((uint32_t)data_ipi * N_SLOTS_MAX) / data_cnt);
    //new_period = (((uint32_t)data_ipi * N_SLOTS_MAX) / data_cnt) * n_pkts / n_slots;
    // check for saturation
    if (new_period < PERIOD_MIN) {
        // T_opt is smaller than PERIOD_MIN
        DEBUG_PRINT_WARNING("network saturated!");
        return PERIOD_MIN;
    }
    // limit the period
    if (new_period > PERIOD_MAX) {
        return PERIOD_MAX;
    }
    return new_period;
}


/**
 * @brief searches the list node_list/stream_list for the node_id/stream_id
 * @param[in] node_id the node ID to search for
 * @param[in] stream_id the stream ID to search for
 * @param[in] node_list the list of node IDs to search
 * @param[in] stream_list the list of stream IDs to search
 * @param[in] list_len the number of entries in the node and stream list
 * @return one if the node_id/stream_id exists in the list, zero otherwise
 * @remark this search could be optimized as the node ids in the list are supposed to be sorted in increasing order
 */
static inline uint8_t sched_is_stream_in_list(uint16_t node_id, uint8_t stream_id, const uint16_t* node_list, const uint8_t* stream_list, uint8_t list_len) {
    while (list_len) {
        if (*node_list == node_id && *stream_list == stream_id) {
            return 1;
        }
        node_list++;
        stream_list++;    
        list_len--;
    }
    return 0;
}


/**
 * @brief compute (and compress) the new schedule
 * @param[in,out] sched the old schedule and the output buffer for the new schedule
 * @param[in] streams_to_update the list of streams of the last round 
 * @param[in] reserve_slot_host set this parameter to one to reserve the first slot of the next schedule for the host
 * @return the size of the new (compressed) schedule
 */
uint16_t sched_compute(schedule_t * const sched, const uint8_t * const streams_to_update, uint8_t reserve_slot_host) {
    
    static uint16_t slots_tmp[N_SLOTS_MAX];
        
    // reset values
    data_ipi = 1;
    data_cnt = 0;
    first_index = 0; 
    n_slots_assigned = 0;
    sched->host_id = node_id;        // embed the node id of the host
    
    // loop through all the streams in the list
#ifndef SCHEDULER_USE_XMEM
    memset(streams, 0, sizeof(streams));        // clear the content of the stream list

    stream_info_t *curr_stream = list_head(streams_list);
    while (curr_stream != NULL) {

        if (sched_is_stream_in_list(curr_stream->node_id, curr_stream->stream_id, sched->slot, streams_to_update, SCHED_N_SLOTS(sched))) {
            curr_stream->n_cons_missed = 0;
        } else if (curr_stream->n_cons_missed & 0x80) {     // no packet received from this stream
            curr_stream->n_cons_missed &= 0x7f;             // clear the last bit
            curr_stream->n_cons_missed++;
        }
        if (curr_stream->n_cons_missed > N_CONS_MISSED_MAX) {
            // too many consecutive slots without reception: delete this stream
            stream_info_t *stream_to_remove = curr_stream;
            curr_stream = curr_stream->next;
            sched_del_stream(stream_to_remove);
        } else {
            uint16_t curr_gcd = gcd(data_ipi, curr_stream->ipi);
            uint16_t k1 = curr_stream->ipi / curr_gcd;
            uint16_t k2 = data_ipi / curr_gcd;
            data_cnt = data_cnt * k1 + k2;
            data_ipi = data_ipi * k1;
            curr_stream = curr_stream->next;
        }
    }

#else
    uint32_t stream_addr = streams_list;
    stream_info_t curr_stream;
    while (stream_addr != MEMBX_INVALID_ADDR) {
    
        fram_read(stream_addr, sizeof(stream_info_t), (uint8_t*)&curr_stream);

        if (sched_is_stream_in_list(curr_stream.node_id, curr_stream.stream_id, sched->slot, streams_to_update, SCHED_N_SLOTS(sched))) {
            curr_stream.n_cons_missed = 0;
            fram_write(stream_addr, sizeof(stream_info_t), (uint8_t*)&curr_stream);     // save changes
        } else if (curr_stream.n_cons_missed & 0x80) {
            curr_stream.n_cons_missed &= 0x7f;
            curr_stream.n_cons_missed++;
            fram_write(stream_addr, sizeof(stream_info_t), (uint8_t*)&curr_stream);     // save changes
        }
        
        if (curr_stream.n_cons_missed > N_CONS_MISSED_MAX) {
            // too many consecutive slots without reception: delete this stream
            sched_del_stream(stream_addr);
        } else {
            uint16_t curr_gcd = gcd(data_ipi, curr_stream.ipi);
            uint16_t k1 = curr_stream.ipi / curr_gcd;
            uint16_t k2 = data_ipi / curr_gcd;
            data_cnt = data_cnt * k1 + k2;
            data_ipi = data_ipi * k1;
        }
        
        if (stream_addr == curr_stream.next) {  // prevent endless loop
            DEBUG_PRINT_WARNING("unexpected stream address!");
            break;
        }
        stream_addr = curr_stream.next;            
    }
#endif // SCHEDULER_USE_XMEM

    memset(sched->slot, 0, sizeof(sched->slot));    // clear the content of the schedule (do NOT move this line further above!)
    
    // assign slots to the host
    if (reserve_slot_host) {
        DEBUG_PRINT_INFO("assigning a slot to the host");
        sched->slot[0] = 0;    // 0 is the host
        n_slots_assigned++;
        sched_stats.t_last_req = time;
    }
    
    period = sched_adapt_period();        // adapt the round period

    time += period;     // increment time by the current period

    if (n_streams == 0) {
        // no streams to process
        goto set_schedule;
    }

    // random initial position in the list
    uint16_t rand_init_pos = (random_rand() >> 1) % n_streams;
    uint16_t i;
    
#ifndef SCHEDULER_USE_XMEM
    curr_stream = list_head(streams_list);
    // make curr_stream point to the random initial position
    for (i = 0; i < rand_init_pos; i++) {
        curr_stream = curr_stream->next;
    }
    // initial stream being processed
    stream_info_t *init_stream = curr_stream;
    do {
        // assign slots for this stream, if possible
        if ((n_slots_assigned < N_SLOTS_MAX) && (time >= (curr_stream->ipi + curr_stream->last_assigned))) {

    // the number of slots to assign to curr_stream
            uint16_t to_assign = (time - curr_stream->last_assigned) / curr_stream->ipi;  // elapsed time / period
            if (period == PERIOD_MIN) {     // if saturated 
                if (curr_stream->next == init_stream || (curr_stream->next == NULL && rand_init_pos == 0)) {
                        // last random stream: assign all possible slots
                } else {
                    // ensure fairness among source nodes when the bandwidth saturates
                    // assigned a number of slots proportional to (1/IPI)
                    uint16_t slots_ipi = period / curr_stream->ipi;
                    if (to_assign > slots_ipi) {
                        to_assign = slots_ipi;
                        if (to_assign == 0 && curr_stream == init_stream) {
                            // first random stream: assign one slot to it, even if it has very long IPI
                            to_assign = 1;
                        }
                    }
                }
            }

            if (to_assign > N_SLOTS_MAX - n_slots_assigned) {
                to_assign = N_SLOTS_MAX - n_slots_assigned;     // limit
            }
            curr_stream->last_assigned += to_assign * curr_stream->ipi;
            for (; to_assign > 0; to_assign--, n_slots_assigned++) {
                slots_tmp[n_slots_assigned] = curr_stream->node_id;
                streams[n_slots_assigned] = curr_stream;
            }
            curr_stream->n_cons_missed |= 0x80; // set the last bit, we are expecting a packet from this stream in the next round
        }

        // go to the next stream in the list
        curr_stream = curr_stream->next;
        if (curr_stream == NULL) {
            // end of the list: start again from the head of the list
            curr_stream = list_head(streams_list);
            first_index = n_slots_assigned; 
        }
    } while (curr_stream != init_stream);

#else

    if (streams_list == MEMBX_INVALID_ADDR) {
        DEBUG_PRINT_WARNING("unexpected invalid stream address");
        goto set_schedule;
    }
    stream_addr = streams_list;
    // make curr_stream point to the random initial position
    for (i = 0; i < rand_init_pos && stream_addr != MEMBX_INVALID_ADDR; i++) {
        fram_read(stream_addr, sizeof(stream_info_t), (uint8_t*)&curr_stream);
        stream_addr = curr_stream.next;
        if (stream_addr == MEMBX_INVALID_ADDR) {
            DEBUG_PRINT_WARNING("unexpected invalid stream address");
            stream_addr = streams_list;
            break;
        }
    }
    uint32_t init_stream = stream_addr;
    do {
        fram_read(stream_addr, sizeof(stream_info_t), (uint8_t*)&curr_stream);

        // assign slots for this stream, if possible
        if ((n_slots_assigned < N_SLOTS_MAX) && (time >= (curr_stream.ipi + curr_stream.last_assigned))) {

        // the number of slots to assign to curr_stream
            uint16_t to_assign = (time - curr_stream.last_assigned) / curr_stream.ipi;  // elapsed time / period
            if (period == PERIOD_MIN) {     // if saturated 
                if (curr_stream.next == init_stream || (curr_stream.next == MEMBX_INVALID_ADDR && rand_init_pos == 0)) {
                    // last random stream: assign all possible slots
                } else {
                    // ensure fairness among source nodes when the bandwidth saturates
                    // assigned a number of slots proportional to (1/IPI)
                    uint16_t slots_ipi = period / curr_stream.ipi;
                    if (to_assign > slots_ipi) {
                        to_assign = slots_ipi;
                        if (to_assign == 0 && init_stream == stream_addr) {
                            // first random stream: assign one slot to it, even if it has very long IPI
                            to_assign = 1;
                        }
                    }
                }
            }
            if (to_assign > N_SLOTS_MAX - n_slots_assigned) {
                to_assign = N_SLOTS_MAX - n_slots_assigned;     // limit
            }
            curr_stream.last_assigned += to_assign * curr_stream.ipi;
            for (; to_assign > 0; to_assign--, n_slots_assigned++) {
                slots_tmp[n_slots_assigned] = curr_stream.node_id;
            }
            curr_stream.n_cons_missed |= 0x80; // set the last bit, we are expecting a packet from this stream in the next round
    
            fram_write(stream_addr, sizeof(stream_info_t), (uint8_t*)&curr_stream); // save changes
        }

        // go to the next stream in the list
        stream_addr = curr_stream.next;
        if (stream_addr == MEMBX_INVALID_ADDR) {
            // end of the list: start again from the head of the list
            stream_addr = streams_list;
            first_index = n_slots_assigned; 
        }        
    } while (stream_addr != init_stream);
#endif  // SCHEDULER_USE_XMEM
    
    // copy into new data structure to keep the node IDs ordered
    memcpy(&sched->slot[reserve_slot_host], &slots_tmp[first_index], (n_slots_assigned - first_index) * sizeof(sched->slot[0]));
    memcpy(&sched->slot[n_slots_assigned - first_index + reserve_slot_host], slots_tmp, first_index * sizeof(sched->slot[0]));
    
set_schedule:
    sched->n_slots = n_slots_assigned;

    if (n_pending_sack) {
        SCHED_SET_SACK_SLOT(sched);
    }
    if ((time < (sched_stats.t_last_req + T_NO_REQ)) || (time >= (sched_stats.t_last_cont + T_NO_REQ)) || !data_cnt) {
        // schedule a contention slot
        sched_stats.t_last_cont = time;
        SCHED_SET_CONT_SLOT(sched);
    }
    
#ifdef COMPRESS_SCHEDULE
    uint8_t compressed_size = sched_compress((uint8_t*)sched->slot, n_slots_assigned);
    if ((compressed_size + SCHED_HEADER_LENGTH) > PACKET_LEN_MAX) {
        DEBUG_PRINT_ERROR("compressed schedule is too big!");
    }
#else // COMPRESS_SCHEDULE
    uint8_t compressed_size = n_slots_assigned * 2;
#endif

    // this schedule is sent at the end of a round: do not communicate (i.e. do not set the first bit of period)
    sched->period = period;   // no need to clear the last bit
    sched->time   = time - (period - 1);
    
    // log the parameters of the new schedule
    DEBUG_PRINT_INFO("schedule updated (s=%u T=%u n=%u|%u l=%u|%u)", n_streams, sched->period, n_slots_assigned, sched->n_slots >> 6, compressed_size, n_slots_assigned * 2);
    
    return compressed_size + SCHED_HEADER_LENGTH;
}


/**
 * @brief initializes the schedule
 * resets all the data structures and sets the initial values
 * @param[out] the schedule
 * @return the size of the (empty) schedule
 */
uint16_t sched_init(schedule_t* sched) {
    // initialize streams member and list
#ifndef SCHEDULER_USE_XMEM
    memb_init(&streams_memb);
    list_init(streams_list);
#else
    membx_init(&streams_memb);
    streams_list = MEMBX_INVALID_ADDR;
#endif // SCHEDULER_USE_XMEM

    // initialize persistent variables
    data_ipi = 1;
    data_cnt = 0;
    n_streams = 0;
    n_slots_assigned = 0;
    n_pending_sack = 0;
    time = 0;                           // global time starts now
    sched->host_id = node_id;           // embed the host ID
    period = PERIOD_IDLE;               // set the period to the minimum at the beginning
    sched->n_slots = n_slots_assigned;  // no data slots
    SCHED_SET_CONT_SLOT(sched);         // include a contention slot
    sched_stats.t_last_cont = time;
    sched_stats.t_last_req  = -T_NO_REQ;
    sched->time = time;
    sched->period = period;
    SCHED_SET_AS_1ST(sched);            // mark as the first schedule (beginning of a round)
    
    return SCHED_HEADER_LENGTH;         // empty schedule, not slots allocated yet
}
