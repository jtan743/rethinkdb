#ifndef __TXT_MEMCACHED_HANDLER_TCC__
#define __TXT_MEMCACHED_HANDLER_TCC__

#include <string.h>
#include "cpu_context.hpp"
#include "event_queue.hpp"
#include "request_handler/txt_memcached_handler.hpp"
#include "conn_fsm.hpp"
#include "corefwd.hpp"

#define DELIMS " \t\n\r"
#define MALFORMED_RESPONSE "ERROR\r\n"
#define UNIMPLEMENTED_RESPONSE "SERVER_ERROR functionality not supported\r\n"
#define STORAGE_SUCCESS "STORED\r\n"
#define STORAGE_FAILURE "NOT_STORED\r\n"
#define NOT_FOUND "NOT_FOUND\r\n"
#define DELETE_SUCCESS "DELETED\r\n"
#define RETRIEVE_TERMINATOR "END\r\n"
#define BAD_BLOB "CLIENT_ERROR bad data chunk\r\n"

#define MAX_MESSAGE_SIZE 200


// Please read and understand the memcached protocol before modifying this
// file. If you only do a cursory readthrough, please check with someone who
// has read it in depth before comitting.

// TODO: if we receive a small request from the user that can be
// satisfied on the same CPU, we should probably special case it and
// do it right away, no need to send it to itself, process it, and
// then send it back to itself.

// Process commands received from the user
template<class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::parse_request(event_t *event)
{
    conn_fsm_t *fsm = (conn_fsm_t*)event->state;
    char *rbuf = fsm->rbuf; 
    unsigned int size = fsm->nrbuf;
    parse_result_t res;

    // TODO: we might end up getting a command, and a piece of the
    // next command. It also means that we can't use one buffer
    //for both recv and send, we need to add a send buffer
    // (assuming we want to support out of band  commands).
    
    // check if we're supposed to be reading a binary blob
    if (loading_data) {
        return read_data(rbuf, size, fsm);
    }

    // Find the first line in the buffer
    // This is only valid if we are not reading binary data
    char *line_end = (char *)memchr(rbuf, '\n', size);
    if (line_end == NULL) {   //make sure \n is in the buffer
        return req_handler_t::op_partial_packet;    //if \n is at the beginning of the buffer, or if it is not preceeded by \r, the request is malformed
    }
    unsigned int line_len = line_end - rbuf + 1;


    if (line_end == rbuf || line_end[-1] != '\r') {
        fsm->consume(line_len);
        return malformed_request(fsm);
    }

    // if we're not reading a binary blob, then the line will be a string - let's null terminate it
    *line_end = '\0';

    // get the first token to determine the command
    char *state;
    char *cmd_str = strtok_r(rbuf, DELIMS, &state);

    if(cmd_str == NULL) {
        fsm->consume(line_len);
        return malformed_request(fsm);
    }
    
    // Execute command
    if(!strcmp(cmd_str, "quit")) {
        // Make sure there's no more tokens
        if (strtok_r(NULL, DELIMS, &state)) {  //strtok will return NULL if there are no more tokens
            fsm->consume(line_len);
            return malformed_request(fsm);
        }
        // Quit the connection
        fsm->consume(fsm->nrbuf);
        return req_handler_t::op_req_quit;

    } else if(!strcmp(cmd_str, "shutdown")) {
        // Make sure there's no more tokens
        if (strtok_r(NULL, DELIMS, &state)) {  //strtok will return NULL if there are no more tokens
            fsm->consume(line_len);
            return malformed_request(fsm);
        }
        // Shutdown the server
        // clean out the rbuf
        fsm->consume(fsm->nrbuf);
        return req_handler_t::op_req_shutdown;
    } else if(!strcmp(cmd_str, "stats")) {
            return issue_stats_request(fsm, line_len);
    } else if(!strcmp(cmd_str, "set")) {     // check for storage commands
            res = parse_storage_command(SET, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "add")) {
            return parse_storage_command(ADD, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "replace")) {
            return parse_storage_command(REPLACE, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "append")) {
            return parse_storage_command(APPEND, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "prepend")) {
            return parse_storage_command(PREPEND, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "cas")) {
            return parse_storage_command(CAS, state, line_len, fsm);

    } else if(!strcmp(cmd_str, "get")) {    // check for retrieval commands
            return get(state, false, line_len, fsm);
    } else if(!strcmp(cmd_str, "gets")) {
            return get(state, true, line_len, fsm);

    } else if(!strcmp(cmd_str, "delete")) {
        return remove(state, line_len, fsm);

    } else if(!strcmp(cmd_str, "incr")) {
        return parse_adjustment(true, state, line_len, fsm);
    } else if(!strcmp(cmd_str, "decr")) {
        return parse_adjustment(false, state, line_len, fsm);
    } else {
        // Invalid command
        fsm->consume(line_len);
        return malformed_request(fsm);
    }

    if (loading_data && fsm->nrbuf > 0) {
        assert(res == req_handler_t::op_partial_packet);
        return parse_request(event);
    } else {
        return res;
    }
}

template <class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::parse_adjustment(bool increment, char *state, unsigned int line_len, conn_fsm_t *fsm) {
    char *key_tmp = strtok_r(NULL, DELIMS, &state);
    char *value_str = strtok_r(NULL, DELIMS, &state);
    char *noreply_str = strtok_r(NULL, DELIMS, &state); //optional
    if (key_tmp == NULL || value_str == NULL) {
        fsm->consume(line_len);
        return malformed_request(fsm);
    }

    bool noreply = false;
    if (noreply_str != NULL) {
        if (!strcmp(noreply_str, "noreply")) {
            noreply = true;
        } else {
            fsm->consume(line_len);
            return malformed_request(fsm);
        }
    }

    node_handler::str_to_key(key_tmp, key);

    fsm->consume(line_len);
    
    long long delta = atoll(value_str);
    
    btree_incr_decr_fsm_t *btree_fsm = new btree_incr_decr_fsm_t(key, increment, delta);
    btree_fsm->noreply = noreply;
    
    fsm->current_request = new request_t(fsm);
    dispatch_btree_fsm(fsm, btree_fsm);

    fsm->consume(this->bytes+2);

    if (noreply)
        return req_handler_t::op_req_parallelizable;
    else
        return req_handler_t::op_req_complex;
}

template <class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::parse_storage_command(storage_command command, char *state, unsigned int line_len, conn_fsm_t *fsm) {
    char *key_tmp = strtok_r(NULL, DELIMS, &state);
    char *flags_str = strtok_r(NULL, DELIMS, &state);
    char *exptime_str = strtok_r(NULL, DELIMS, &state);
    char *bytes_str = strtok_r(NULL, DELIMS, &state);
    char *cas_unique_str = NULL;
    if (command == CAS)
        cas_unique_str = strtok_r(NULL, DELIMS, &state);
    char *noreply_str = strtok_r(NULL, DELIMS, &state); //optional

    //check for proper number of arguments
    if ((key_tmp == NULL || flags_str == NULL || exptime_str == NULL || bytes_str == NULL || (command == CAS && cas_unique_str == NULL))) {
        fsm->consume(line_len);
        return malformed_request(fsm);
    }

    cmd = command;
    node_handler::str_to_key(key_tmp, key);

    char *invalid_char;
    flags = strtoul(flags_str, &invalid_char, 10);  //a 32 bit integer.  int alone does not guarantee 32 bit length
    if (*invalid_char != '\0') {  // ensure there were no improper characters in the token - i.e. parse was successful
        fsm->consume(line_len);
        return malformed_request(fsm);
    }

    exptime = strtoul(exptime_str, &invalid_char, 10);
    if (*invalid_char != '\0') {
        fsm->consume(line_len);
        return malformed_request(fsm);
    }

    bytes = strtoul(bytes_str, &invalid_char, 10);
    if (*invalid_char != '\0') {
        fsm->consume(line_len);
        return malformed_request(fsm);
    }

    if (cmd == CAS) {
        cas_unique = strtoull(cas_unique_str, &invalid_char, 10);
        if (*invalid_char != '\0') {
            fsm->consume(line_len);
            return malformed_request(fsm);
        }
    }

    this->noreply = false;
    if (noreply_str != NULL) {
        if (!strcmp(noreply_str, "noreply")) {
            this->noreply = true;
        } else {
            fsm->consume(line_len);
            return malformed_request(fsm);
        }
    }

    fsm->consume(line_len); //consume the line
    loading_data = true;
    return read_data(fsm->rbuf, fsm->nrbuf, fsm);
}
	
template <class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::read_data(char *data, unsigned int size, conn_fsm_t *fsm) {
    check("memcached handler should be in loading data state", !loading_data);
    if (size < bytes + 2){//check that the buffer contains enough data.  must also include \r\n
        return req_handler_t::op_partial_packet;
    }
    loading_data = false;
    fsm->consume(bytes+2);
    if (data[bytes] != '\r' || data[bytes+1] != '\n') {
        write_msg(fsm, BAD_BLOB);
        return req_handler_t::op_malformed;
    }

    btree_fsm_t *btree_fsm;
    switch(cmd) {
        case SET:
            btree_fsm = new btree_set_fsm_t(key, data, bytes, true, true);
            break;
        case ADD:
            btree_fsm = new btree_set_fsm_t(key, data, bytes, true, false);
            break;
        case REPLACE:
            btree_fsm = new btree_set_fsm_t(key, data, bytes, false, true);
            break;
        case APPEND:
        case PREPEND:
        case CAS:
            return unimplemented_request(fsm);
        default:
            return malformed_request(fsm);
    }
    
    btree_fsm->noreply = noreply;
    
    fsm->current_request = new request_t(fsm);
    // Keep track of cmd so we can reply appropriately
    fsm->current_request->handler_data = (void*)new int(cmd);

    dispatch_btree_fsm(fsm, btree_fsm);

    if (noreply)
        return req_handler_t::op_req_parallelizable;
    else
        return req_handler_t::op_req_complex;
}

template <class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::malformed_request(conn_fsm_t *fsm) {
    write_msg(fsm, MALFORMED_RESPONSE);
    return req_handler_t::op_malformed;
}

template <class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::unimplemented_request(conn_fsm_t *fsm) {
    write_msg(fsm, UNIMPLEMENTED_RESPONSE);
    return req_handler_t::op_malformed;
}

template <class config_t>
void txt_memcached_handler_t<config_t>::write_msg(conn_fsm_t *fsm, const char *str) {
    fsm->sbuf->append(str, strlen(str) + 1);
}

template <class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::get(char *state, bool include_unique, unsigned int line_len, conn_fsm_t *fsm) {
    
    if (include_unique)
        return unimplemented_request(fsm);
        
    // Create request
    fsm->current_request = new request_t(fsm);
    
    char *key_str = strtok_r(NULL, DELIMS, &state);
    if (key_str == NULL) return malformed_request(fsm);
    
    do {
        // See if we can fit one more request
        if(fsm->current_request->nstarted == MAX_OPS_IN_REQUEST) {
            // We can't fit any more operations, let's just break
            // and complete the ones we already sent out to other
            // cores.
            break;

            // TODO: to a user, it will look like some of his
            // requests aren't satisfied. We need to notify them
            // somehow.
        }

        node_handler::str_to_key(key_str, key);

        // Ok, we've got a key, initialize the FSM and add it to
        // the request
        btree_get_fsm_t *btree_fsm = new btree_get_fsm_t(key);
        dispatch_btree_fsm(fsm, btree_fsm);
        
        key_str = strtok_r(NULL, DELIMS, &state);
        
    } while(key_str);

    //clean out the rbuf
    fsm->consume(line_len);
    return req_handler_t::op_req_complex;
}


template <class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::remove(char *state, unsigned int line_len, conn_fsm_t *fsm) {
    char *key_str = strtok_r(NULL, DELIMS, &state);
    if (key_str == NULL) {
        fsm->consume(line_len);
        return malformed_request(fsm);
    }

    unsigned long time = 0;
    this->noreply = false;
    char *time_or_noreply_str = strtok_r(NULL, DELIMS, &state);
    if (time_or_noreply_str != NULL) {
        if (!strcmp(time_or_noreply_str, "noreply")) {
            this->noreply = true;
        } else { //must represent a time, then
            char *invalid_char;
            time = strtoul(time_or_noreply_str, &invalid_char, 10);
            if (*invalid_char != '\0')  // ensure there were no improper characters in the token - i.e. parse was successful
                return unimplemented_request(fsm);

            // see if there's a noreply arg too
            char *noreply_str = strtok_r(NULL, DELIMS, &state);
            if (noreply_str != NULL) {
                if (!strcmp(noreply_str, "noreply")) {
                    this->noreply = true;
                } else {
                    fsm->consume(line_len);
                    return malformed_request(fsm);
                }
            }
        }
    }

    node_handler::str_to_key(key_str, key);

    // Create request
    btree_delete_fsm_t *btree_fsm = new btree_delete_fsm_t(key);
    btree_fsm->noreply = this->noreply;

    fsm->current_request = new request_t(fsm);
    dispatch_btree_fsm(fsm, btree_fsm);

    //clean out the rbuf
    fsm->consume(line_len);

    if (this->noreply)
        return req_handler_t::op_req_parallelizable;
    else
        return req_handler_t::op_req_complex;
}

template<class config_t>
void txt_memcached_handler_t<config_t>::build_response(request_t *request) {
    // Since we're in the middle of processing a command,
    // fsm->buf must exist at this point.
    conn_fsm_t *fsm = request->netfsm;
    btree_get_fsm_t *btree_get_fsm = NULL;
    btree_set_fsm_t *btree_set_fsm = NULL;
    btree_delete_fsm_t *btree_delete_fsm = NULL;
    int count;
    char tmpbuf[MAX_MESSAGE_SIZE];
    
    assert(request->nstarted > 0 && request->nstarted == request->ncompleted);

    if (request->msgs[0]->type == cpu_message_t::mt_btree)
    {
        btree_fsm_t *btree = (btree_fsm_t*)(request->msgs[0]);
        switch(btree->fsm_type) {
            case btree_fsm_t::btree_get_fsm:
                // TODO: make sure we don't overflow the buffer with sprintf
                for(unsigned int i = 0; i < request->nstarted; i++) {
                    btree_get_fsm = (btree_get_fsm_t*)request->msgs[i];
                    if(btree_get_fsm->op_result == btree_get_fsm_t::btree_found) {
                        //TODO: support flags
                        btree_key *key = btree_get_fsm->key;
                        btree_value *value = btree_get_fsm->value;
                        count = snprintf(tmpbuf, MAX_MESSAGE_SIZE, "VALUE %*.*s %u %u\r\n%*.*s\r\n", key->size, key->size, key->contents, 0, value->size, value->size, value->size, value->contents);
                        check ("Too big of a message, increase MAX_MESSAGE_SIZE", count == MAX_MESSAGE_SIZE);
                        fsm->sbuf->append(tmpbuf, count);
                    } else if(btree_get_fsm->op_result == btree_get_fsm_t::btree_not_found) {
                        // do nothing
                    }
                }
                count = sprintf(tmpbuf, RETRIEVE_TERMINATOR);
                check ("Too big of a message, increase MAX_MESSAGE_SIZE", count == MAX_MESSAGE_SIZE);
                fsm->sbuf->append(tmpbuf, count);
                break;

            case btree_fsm_t::btree_set_fsm:
                // For now we only support one set operation at a time
                assert(request->nstarted == 1);

                btree_set_fsm = (btree_set_fsm_t*)request->msgs[0];

                if (btree_set_fsm->noreply) {
                    ;// if noreply is set do not reply regardless of success or failure
                } else {
                    // noreply not set, send reply depending on type of request

                    switch(cmd) {
                        case btree_set_type_incr:
                        case btree_set_type_decr:
                            if (btree_set_fsm->set_was_successful) {
                                strncpy(tmpbuf, btree_set_fsm->get_value()->contents, btree_set_fsm->get_value()->size);
                                tmpbuf[btree_set_fsm->get_value()->size + 0] = '\r';
                                tmpbuf[btree_set_fsm->get_value()->size + 1] = '\n';
                                fsm->sbuf->append(tmpbuf, btree_set_fsm->get_value()->size + 2);
                            } else {
                                fsm->sbuf->append((char *) NOT_FOUND, strlen(NOT_FOUND));
                            }
                            break;

                        case btree_set_type_set:
                        case btree_set_type_add:
                        case btree_set_type_replace:
                            if (btree_set_fsm->set_was_successful) {
                                fsm->sbuf->append((char *) STORAGE_SUCCESS, strlen(STORAGE_SUCCESS));
                            } else {
                                fsm->sbuf->append((char *) STORAGE_FAILURE, strlen(STORAGE_FAILURE));
                            }
                    }
                    break;

                    case btree_fsm_t::btree_delete_fsm:
                    // For now we only support one delete operation at a time
                    assert(request->nstarted == 1);

                    btree_delete_fsm = (btree_delete_fsm_t*)request->msgs[0];

                    if(btree_delete_fsm->op_result == btree_delete_fsm_t::btree_found) {
                        fsm->sbuf->append((char *) DELETE_SUCCESS, strlen(DELETE_SUCCESS));
                    } else if (btree_delete_fsm->op_result == btree_delete_fsm_t::btree_not_found) {
                        fsm->sbuf->append((char *) NOT_FOUND, strlen(NOT_FOUND));
                    } else {
                        check("memchached_handler_t::build_response - Uknown value for btree_delete_fsm->op_result\n", 0);
                        break;
                    }

                    default:
                    check("txt_memcached_handler_t::build_response - Unknown btree op", 0);
                    break;
                }
        }

        delete request;
    } else if (request->msgs[0]->type == cpu_message_t::mt_perfmon)
    {
        // Combine all responses into one
        perfmon_t combined_perfmon;
        for(int i = 0; i < (int)request->ncompleted; i++) {
            perfmon_msg_t *_msg = (perfmon_msg_t*)request->msgs[i];
            combined_perfmon.accumulate(_msg->perfmon);
        }
        
        // Print the resultings perfmon
        perfmon_t::perfmon_map_t *registry = &combined_perfmon.registry;
        for(perfmon_t::perfmon_map_t::iterator iter = registry->begin(); iter != registry->end(); iter++)
        {
            // TODO: make sure we don't overflow the sbuf
            fsm->sbuf->append("STAT ", 5);
            
            int name_len = strlen(iter->first);
            fsm->sbuf->append(iter->first, name_len);
            fsm->sbuf->append(" ", 1);

            char tmpbuf[10];
            int val_len = iter->second.print(tmpbuf, 10);
            fsm->sbuf->append(tmpbuf, val_len);

            fsm->sbuf->append("\r\n", 2);
        }
    }
    fsm->current_request = NULL;
}

template <class config_t>
typename txt_memcached_handler_t<config_t>::parse_result_t txt_memcached_handler_t<config_t>::issue_stats_request(conn_fsm_t *fsm, unsigned int line_len) {

    int nworkers = (int)get_cpu_context()->event_queue->parent_pool->nworkers;
    int id = get_cpu_context()->event_queue->queue_id;

    // Tell every single CPU core to pass their perfmon module *by copy*
    // to this CPU
    request_t *request = new request_t(fsm);
    for (int i = 0; i < nworkers; i++)
    {
        perfmon_msg_t *perfmon_req_msg = new perfmon_msg_t(request);
        perfmon_req_msg->return_cpu = id;
        request->msgs[i] = perfmon_req_msg;
        req_handler_t::event_queue->message_hub.store_message(i, perfmon_req_msg);
    }    
    request->nstarted = nworkers;
    fsm->current_request = request;
    fsm->consume(line_len);
    return req_handler_t::op_req_complex;    
}

#endif // __MEMCACHED_HANDLER_TCC__
