// Copyright 2017 The Grin Developers
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Functions and definitions specific to cuckoo-miner's modifications, 
// localising as many changes as possible here to avoid diverging from 
// the original source

#ifndef CUCKOO_MINER_H
#define CUCKOO_MINER_H

#include <stdio.h>
#include <string.h>
#include <pthread.h>
//Just for debug output when printf is squashed
#include <iostream>

#ifdef __APPLE__
#include "osx_barrier.h"
#endif

#include "hash_impl.h"
#include "concurrentqueue.h"

#define SQUASH_OUTPUT 1

#if SQUASH_OUTPUT
#define printf(fmt, ...) (0)
#endif

#define HASH_LENGTH 32
int MAX_QUEUE_SIZE=1000;

/** 
 * Some hardwired stuff to hold properties
 * without dynamically allocating memory
 * kept very simple for now
 */

#define MAX_NUM_PROPERTIES 16
#define MAX_PROPERTY_NAME_LENGTH 64
#define MAX_PROPERTY_DESC_LENGTH 256

#define PROPERTY_RETURN_OK 0
#define PROPERTY_RETURN_NOT_FOUND 1
#define PROPERTY_RETURN_OUTSIDE_RANGE 2
#define PROPERTY_RETURN_BUFFER_TOO_SMALL 3

int allocated_properties=0;

struct PLUGIN_PROPERTY {
    char name[MAX_PROPERTY_NAME_LENGTH];
    char description[MAX_PROPERTY_DESC_LENGTH];
    u32 default_value;
    u32 min_value;
    u32 max_value;
};

PLUGIN_PROPERTY PROPS[MAX_NUM_PROPERTIES];

void add_plugin_property(PLUGIN_PROPERTY new_property){
    if (allocated_properties>MAX_NUM_PROPERTIES-1){
        return;
    }
    PROPS[allocated_properties++]=new_property;
}

/*
 * Either fills given string with properties, or returns error code
 * if there isn't enough buffer
 */

int get_properties_as_json(char* prop_string, int* length){
    int remaining=*length;
    const char* property_json = "{\"name\":\"%s\",\"description\":\"%s\",\"default_value\":%d,\"min_value\":%d,\"max_value\":%d}";
    //minimum return is "[]\0"
    if (remaining<=3){
        //TODO: Meaningful return code
        return PROPERTY_RETURN_BUFFER_TOO_SMALL;
    }
    prop_string[0]='[';
    int last_write_pos=1;
    for (int i=0;i<allocated_properties;i++){
        int last_written=snprintf(prop_string+last_write_pos, 
                              remaining, 
                              property_json, PROPS[i].name, 
                              PROPS[i].description, PROPS[i].default_value,
                              PROPS[i].min_value, PROPS[i].max_value);
        remaining-=last_written;
        last_write_pos+=last_written;
        //no room for anything else, comma or trailing ']'
        if (remaining<2){
            //TODO: meaningful error code
            return PROPERTY_RETURN_BUFFER_TOO_SMALL;
        }
        //write comma
        if (i<allocated_properties-1){
            //overwrite trailing \0 in this case
            prop_string[last_write_pos++]=',';
        } 
    }
    //write final characters
    if (remaining<2){
        return PROPERTY_RETURN_BUFFER_TOO_SMALL;
    }
    //overwrite trailing \0
    prop_string[last_write_pos]=']';
    prop_string[last_write_pos+1]='\0';
    remaining -=2;
    *length=last_write_pos+1;
    
    //empty set
    if (*length==3){
        *length=2;
    }
    return PROPERTY_RETURN_OK;
}

/**
 * Replace the SHA256 function with our own, pulled from secp256k
 */

void SHA256(unsigned char * in, u32 len, unsigned char* out){
    secp256k1_sha256_t sha;
    secp256k1_sha256_initialize(&sha);
    secp256k1_sha256_write(&sha, in, len);
    secp256k1_sha256_finalize(&sha, out);
}

//Handy function to keep around for debugging
static void print_buf(const char *title, const unsigned char *buf, size_t buf_len)
{
    size_t i = 0;
    printf("%s\n", title);
    for(i = 0; i < buf_len; ++i)
    printf("%02X%s", buf[i],
             ( i + 1 ) % 16 == 0 ? "\r\n" : " " );
}

//two flags here, because we want to ensure a call to process
//blocks until the processing from the previous call has finished
bool processing_finished=true;
bool internal_processing_finished=true;

bool should_quit=false;

typedef struct queueInput {
    unsigned char nonce[8];
    unsigned char hash[HASH_LENGTH];
    //other identifiers
} QueueInput;

typedef struct queueOutput {
    unsigned char nonce[8];
    u32 result_nonces[42];
    //other identifiers
} QueueOutput;

moodycamel::ConcurrentQueue<QueueInput> INPUT_QUEUE;
moodycamel::ConcurrentQueue<QueueOutput> OUTPUT_QUEUE;

extern "C" int cuckoo_is_queue_under_limit(){
    if (should_quit) return 0;
    if (INPUT_QUEUE.size_approx()<=MAX_QUEUE_SIZE){
        return 1;
    } else {
        return 0;
    }
}

extern "C" int cuckoo_push_to_input_queue(unsigned char* hash, 
                                   int hash_length,
                                   unsigned char* nonce) {
    if (should_quit) return 0;
    QueueInput input;
    memset(input.hash, 0, sizeof(input.hash));
    assert(hash_length <= sizeof(input.hash));
    memcpy(input.hash, hash, hash_length);
    memcpy(input.nonce, nonce, sizeof(input.nonce));
    INPUT_QUEUE.enqueue(input);
    return 1;
}

extern "C" int cuckoo_read_from_output_queue(u32* output, unsigned char* nonce){
    if (should_quit) return 0;
    QueueOutput item;
    bool found = OUTPUT_QUEUE.try_dequeue(item);
    if (found){
        memcpy(nonce, item.nonce, sizeof(item.nonce));
        memcpy(output, item.result_nonces, sizeof(item.result_nonces));
        return 1;
    } else {
        return 0;
    }
}

//forward decs, these should be implemented by
//plugins... this will just return whether
//the plugin is ready to accept the next hash
static bool cuckoo_internal_ready_for_hash();

// and process it, placing any results on the
// output queue
static int cuckoo_internal_process_hash(unsigned char* hash, int hash_length, unsigned char* nonce);

void *cuckoo_process(void *args) {
    should_quit=false;
    while(!should_quit){
        if (!cuckoo_internal_ready_for_hash()) continue;
        QueueInput item;
        bool found = INPUT_QUEUE.try_dequeue(item);
        //std::cout<<"Queue size (approx): "<<INPUT_QUEUE.size_approx()<<std::endl;
        if (found){
            cuckoo_internal_process_hash(item.hash, HASH_LENGTH, item.nonce);
        }

    }

    //empty the queues
    QueueInput in;
    QueueOutput out;
    for (int i=0;i<MAX_QUEUE_SIZE;i++){
        INPUT_QUEUE.try_dequeue(in);
        OUTPUT_QUEUE.try_dequeue(out);
    }
    processing_finished=true;

}

extern "C" int cuckoo_start_processing() {
    printf("Start processing cuckoo process");

    //prevent further runs until the previous processing run is finished, if any
    //while(!internal_processing_finished){};      
    //while(!processing_finished){};
    //should_quit=false;
    //processing_finished=false;
    //internal_processing_finished=false;

    pthread_t cuckoo_process_thread;
    if (!pthread_create(&cuckoo_process_thread, NULL, cuckoo_process, NULL)){
        if (pthread_detach(cuckoo_process_thread)){
            return 1;
        } 
        
    }
}

extern "C" int cuckoo_stop_processing() {
    should_quit=true;    
}

#endif //CUCKOO_MINER_H
