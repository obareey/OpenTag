/* Copyright 2010-2011 JP Norair
  *
  * Licensed under the OpenTag License, Version 1.0 (the "License");
  * you may not use this file except in compliance with the License.
  * You may obtain a copy of the License at
  *
  * http://www.indigresso.com/wiki/doku.php?id=opentag:license_1_0
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
  */
/**
  * @file       OTlib/queue.h
  * @author     JP Norair
  * @version    V1.0
  * @date       15 July 2011
  * @brief      A module and ADT for buffering data packets.
  * @defgroup   Queue (Queue Module)
  * @ingroup    Queue
  *
  * The queue mechanism explicitly specifies two queues: a transmit queue and a 
  * receive queue.  There is no facilty for dynamic queue allocation, which 
  * shouldn't be a problem given the purpose-built, embedded nature of the 
  * platform.  Even so, it wouldn't take a lot of effort to work dynamic 
  * functionality into it.
  ******************************************************************************
  */

#ifndef __QUEUE_H
#define __QUEUE_H

#include "OT_types.h"
//#include "buffers.h"



/** @typedef Queue
  * 
  * The Queue data type does not contain the data in the queues themselves, just
  * information on how to get that data as well as any other useful variables.
  *
  * ot_u16 alloc        Allocation of the queue data, in bytes
  *
  * ot_u16 length       The current extent of the queue data, in bytes
  * Twobytes options    User flags
  * ot_u8* front        First address of queue data
  * ot_u8* back         Used for boundary checking (user adjustable)
  * ot_u8* getcursor    Cursor address for reading from queue
  * ot_u8* putcursor    Cursor address for writing to queue
  */
typedef struct {
    ot_u16      alloc;
    ot_u16      length;
    Twobytes    options;
    ot_u8*      front;
    ot_u8*      back;
    ot_u8*      getcursor;
    ot_u8*      putcursor;
} Queue;


/** @brief Generic initialization routine for Queues.
  * @param q        (Queue*) Pointer to the Queue ADT
  * @param buffer   (ot_u8*) Queue data buffer
  * @param alloc    (ot_u16) allocated bytes for queue
  * @retval none
  * @ingroup Queue
  */
void q_init(Queue* q, ot_u8* buffer, ot_u16 alloc);


/** @brief Reposition the Queue pointers to a new buffer, don't change attributes
  * @param q        (Queue*) Pointer to the Queue ADT
  * @param buffer   (ot_u8*) Queue data buffer
  * @retval none
  * @ingroup Queue
  *
  * Most commonly used when multiple frames are in the same queue.
  */
void q_rebase(Queue* q, ot_u8* buffer);



void q_copy(Queue* q1, Queue* q2);



/** @brief Empties the supplied Queue, but doesn't actually erase data
  * @param q        (Queue*) Pointer to the Queue ADT
  * @retval none
  * @ingroup Queue
  */
void q_empty(Queue* q);


/** @brief Starts a queue by loading in config data
  * @param q        (Queue*) Pointer to the Queue ADT
  * @param offset   (ot_uint) bytes to offset the fist data writes from the front
  * @param options  (ot_u16) option bits.  user-defined usage.
  * @retval ot_u8*  Pointer to queue get & putcursor, or NULL if an error
  * @ingroup Queue
  */
ot_u8* q_start(Queue* q, ot_uint offset, ot_u16 options);



/** @brief Returns the current getcursor position, and then moves it forward
  * @param q        (Queue*) Pointer to the Queue ADT
  * @param shift    (ot_int) bytes to move getcursor forward
  * @retval ot_u8*  Pointer to getcursor at original position
  * @ingroup Queue
  */
ot_u8* q_markbyte(Queue* q, ot_int shift);


/** @brief Writes a byte to a Queue's putcursor, and advances it
  * @param q        (Queue*) Pointer to the Queue ADT
  * @param byte_in  (ot_u8) byte to write
  * @retval none
  * @ingroup Queue
  */
void q_writebyte(Queue* q, ot_u8 byte_in);


/** @brief Writes a 16 bit short integer to the Queue's putcursor, and advances it
  * @param q        (Queue*) Pointer to the Queue ADT
  * @param short_in (ot_u16) Short integer to write
  * @retval none
  * @ingroup Queue
  * @note The _be variant will use big endian, which is fast for copying data
  *       from the UDB, given that the UDB is big endian and DASH7 also uses big
  *       endian for data streams.  The normal variant will do endian conversion
  *       in order to move integers from memory to the queue.
  */
void q_writeshort(Queue* q, ot_u16 short_in);
void q_writeshort_be(Queue* q, ot_u16 short_in);



/** @brief Writes a 32 bit long integer to the Queue's putcursor, and advances it
  * @param q        (Queue*) Pointer to the Queue ADT
  * @param long_in  (ot_u32) Long integer to write
  * @retval none
  * @ingroup Queue
  * @note The _be variant will use big endian, which is fast for copying data
  *       from the UDB, given that the UDB is big endian and DASH7 also uses big
  *       endian for data streams.  The normal variant will do endian conversion
  *       in order to move integers from memory to the queue.
  */
void q_writelong(Queue* q, ot_u32 long_in);



/** @brief Reads a byte at the Queue's getcursor, and advances it
  * @param q        (Queue*) Pointer to the Queue ADT
  * @retval ot_u8   Byte read
  * @ingroup Queue
  */
ot_u8 q_readbyte(Queue* q);


/** @brief Reads a 16 bit short integer at the Queue's getcursor, and advances it
  * @param q        (Queue*) Pointer to the Queue ADT
  * @retval ot_u16  Short integer read.
  * @ingroup Queue
  * @note The _be variant will use big endian, which is fast for copying data
  *       from the UDB, given that the UDB is big endian and DASH7 also uses big
  *       endian for data streams.  The normal variant will do endian conversion
  *       in order to move integers from memory to the queue.
  */
ot_u16 q_readshort(Queue* q);
ot_u16 q_readshort_be(Queue* q);


/** @brief Reads a 32 bit long integer at the Queue's getcursor, and advances it
  * @param q        (Queue*) Pointer to the Queue ADT
  * @retval ot_u32  Long integer read.
  * @ingroup Queue
  * @note The _be variant will use big endian, which is fast for copying data
  *       from the UDB, given that the UDB is big endian and DASH7 also uses big
  *       endian for data streams.  The normal variant will do endian conversion
  *       in order to move integers from memory to the queue.
  */
ot_u32 q_readlong(Queue* q);


void q_writestring(Queue* q, ot_u8* string, ot_int length);
void q_readstring(Queue* q, ot_u8* string, ot_int length);




#endif
