/*
* Copyright(c) 2018 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#include <assert.h>
#include <stdlib.h>
#include "EbSystemResourceManager.h"
#include "EbUtility.h"

/**************************************
 * EbFifoCtor
 **************************************/
static EB_ERRORTYPE EbFifoCtor(
    EbFifo_t           *fifoPtr,
    EB_U32              initialCount,
    EB_U32              maxCount,
    EbObjectWrapper_t  *firstWrapperPtr,
    EbObjectWrapper_t  *lastWrapperPtr,
    EbMuxingQueue_t    *queuePtr)
{
    // Create Counting Semaphore
    EB_CREATESEMAPHORE(EB_HANDLE, fifoPtr->countingSemaphore, sizeof(EB_HANDLE), EB_SEMAPHORE, initialCount, maxCount);

    // Create Buffer Pool Mutex
    EB_CREATEMUTEX(EB_HANDLE, fifoPtr->lockoutMutex, sizeof(EB_HANDLE), EB_MUTEX);

    // Initialize Fifo First & Last ptrs
    fifoPtr->firstPtr           = firstWrapperPtr;
    fifoPtr->lastPtr            = lastWrapperPtr;

    // Copy the Muxing Queue ptr this Fifo belongs to
    fifoPtr->queuePtr           = queuePtr;

    fifoPtr->dbg_info           = NULL;
    return EB_ErrorNone;
}


/**************************************
 * EbFifoPushBack
 **************************************/
static EB_ERRORTYPE EbFifoPushBack(
    EbFifo_t            *fifoPtr,
    EbObjectWrapper_t   *wrapperPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;
    
    //Jing: init the next Ptr
    wrapperPtr->nextPtr = NULL;
    // If FIFO is empty
    if(fifoPtr->firstPtr == (EbObjectWrapper_t*) EB_NULL) {
        fifoPtr->firstPtr = wrapperPtr;
        fifoPtr->lastPtr  = wrapperPtr;
    } else {
        fifoPtr->lastPtr->nextPtr = wrapperPtr;
        fifoPtr->lastPtr = wrapperPtr;
    }

    //fifoPtr->lastPtr->nextPtr = (EbObjectWrapper_t*) EB_NULL;

    return return_error;
}


/**************************************
 * EbFifoPopFront
 **************************************/
static EB_ERRORTYPE EbFifoPopFront(
    EbFifo_t            *fifoPtr,
    EbObjectWrapper_t  **wrapperPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    // Set wrapperPtr to head of BufferPool
    *wrapperPtr = fifoPtr->firstPtr;

    // Update tail of BufferPool if the BufferPool is now empty
    fifoPtr->lastPtr = (fifoPtr->firstPtr == fifoPtr->lastPtr) ? (EbObjectWrapper_t*) EB_NULL: fifoPtr->lastPtr;

    // Update head of BufferPool
    fifoPtr->firstPtr = fifoPtr->firstPtr->nextPtr;

    return return_error;
}

/**************************************
* EbFifoPopFront
**************************************/
static EB_BOOL EbFifoPeakFront(
    EbFifo_t            *fifoPtr)
{

    // Set wrapperPtr to head of BufferPool
    if (fifoPtr->firstPtr == (EbObjectWrapper_t*)EB_NULL) {
        return EB_TRUE;
    }
    else {
        return EB_FALSE; 
    }
}

/**************************************
 * EbCircularBufferCtor
 **************************************/
static EB_ERRORTYPE EbCircularBufferCtor(
    EbCircularBuffer_t  **bufferDblPtr,
    EB_U32                bufferTotalCount)
{
    // Jing: Change to link list
    EB_U32 bufferIndex;
    EbCircularBuffer_t *bufferPtr;

    EB_MALLOC(EbCircularBuffer_t*, bufferPtr, sizeof(EbCircularBuffer_t), EB_N_PTR);

    *bufferDblPtr = bufferPtr;

    bufferPtr->bufferTotalCount = bufferTotalCount;

    EB_MALLOC(EbLinkNode_t*, bufferPtr->arrayPtr, sizeof(EbLinkNode_t) * bufferPtr->bufferTotalCount, EB_N_PTR);

    for(bufferIndex=0; bufferIndex < bufferTotalCount; ++bufferIndex) {
        bufferPtr->arrayPtr[bufferIndex].ptr = EB_NULL;
        bufferPtr->arrayPtr[bufferIndex].nextPtr = &bufferPtr->arrayPtr[(bufferIndex + 1) % bufferTotalCount];
        bufferPtr->arrayPtr[bufferIndex].prevPtr = &bufferPtr->arrayPtr[(bufferIndex + bufferTotalCount - 1) % bufferTotalCount];
    }

    bufferPtr->headPtr = bufferPtr->arrayPtr;
    bufferPtr->tailPtr = bufferPtr->arrayPtr;

    bufferPtr->currentCount = 0;

    return EB_ErrorNone;
}



/**************************************
 * EbCircularBufferEmptyCheck
 **************************************/
static EB_BOOL EbCircularBufferEmptyCheck(
    EbCircularBuffer_t   *bufferPtr)
{
    return ((bufferPtr->headPtr == bufferPtr->tailPtr) && (bufferPtr->headPtr->ptr == EB_NULL)) ? EB_TRUE : EB_FALSE;
}

/**************************************
 * EbCircularBufferPopFront
 **************************************/
static EB_ERRORTYPE EbCircularBufferPopFront(
    EbCircularBuffer_t   *bufferPtr,
    EB_PTR               *objectPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    // Copy the head of the buffer into the objectPtr
    *objectPtr = bufferPtr->headPtr->ptr;
    bufferPtr->headPtr->ptr = EB_NULL;

    // Increment the head & check for rollover
    bufferPtr->headPtr = bufferPtr->headPtr->nextPtr;

    // Decrement the Current Count
    --bufferPtr->currentCount;

    return return_error;
}

/**************************************
 * EbCircularBufferPushBack
 **************************************/
static EB_ERRORTYPE EbCircularBufferPushBack(
    EbCircularBuffer_t   *bufferPtr,
    EbObjectWrapper_t    *objectPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    // Copy the pointer into the array
    bufferPtr->tailPtr->ptr = (EB_PTR)objectPtr;

    // Increment the tail & check for rollover
    bufferPtr->tailPtr = bufferPtr->tailPtr->nextPtr;

    // Increment the Current Count
    ++bufferPtr->currentCount;

    return return_error;
}

// Jing:
// Insert objectPtr according to the rank
// The list will be in an increasing order
static EB_ERRORTYPE EbCircularBufferRankInsert(
    EbCircularBuffer_t   *bufferPtr,
    EbObjectWrapper_t    *objectPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    //Put the value to tailPtr and sort/update the link list
    if (bufferPtr->tailPtr == bufferPtr->headPtr && bufferPtr->tailPtr->ptr == EB_NULL) {
        //empty, when it's full this function should not be called
        bufferPtr->tailPtr->ptr = (EB_PTR)objectPtr;
        bufferPtr->tailPtr = bufferPtr->tailPtr->nextPtr;
    } else {
        EbLinkNode_t* node = bufferPtr->headPtr;
        while (node != bufferPtr->tailPtr) {
            assert(node->ptr != EB_NULL);
            if (objectPtr->rank < ((EbObjectWrapper_t *)node->ptr)->rank) {
                //Insert it before current Node
                break;
            }
            node = node->nextPtr;
        }

        bufferPtr->tailPtr->ptr = (EB_PTR)objectPtr;

        if (node == bufferPtr->tailPtr) {
            bufferPtr->tailPtr = bufferPtr->tailPtr->nextPtr;
        } else {
            //Get tail object out
            EbLinkNode_t* new_tail = bufferPtr->tailPtr->nextPtr;
            bufferPtr->tailPtr->prevPtr->nextPtr = bufferPtr->tailPtr->nextPtr;
            bufferPtr->tailPtr->nextPtr->prevPtr = bufferPtr->tailPtr->prevPtr;

            //Insert it before current node
            bufferPtr->tailPtr->nextPtr = node;
            bufferPtr->tailPtr->prevPtr = node->prevPtr;
            node->prevPtr->nextPtr = bufferPtr->tailPtr;
            node->prevPtr = bufferPtr->tailPtr;

            bufferPtr->tailPtr = new_tail;
            if (node == bufferPtr->headPtr) {
                bufferPtr->headPtr = bufferPtr->headPtr == bufferPtr->tailPtr ? bufferPtr->headPtr:bufferPtr->headPtr->prevPtr;
            }
        }
    }

    //debug, validate the link list
    {
        if (bufferPtr->currentCount == bufferPtr->bufferTotalCount - 2) {
            assert(bufferPtr->tailPtr->nextPtr == bufferPtr->headPtr);
        }
        if (bufferPtr->currentCount == bufferPtr->bufferTotalCount - 1) {
            assert(bufferPtr->tailPtr == bufferPtr->headPtr);
        }

        EbLinkNode_t* hd = bufferPtr->headPtr;
        EbLinkNode_t* hd_prev = bufferPtr->headPtr;
        for (unsigned int i = 0; i < bufferPtr->bufferTotalCount; i++) {
            hd = hd->nextPtr;
            hd_prev = hd_prev->prevPtr;
        }
        assert(hd == bufferPtr->headPtr);
        assert(hd_prev == bufferPtr->headPtr);

        while (hd != bufferPtr->tailPtr) {
            assert(hd->ptr != EB_NULL);
            hd = hd->nextPtr;
        }

        EbLinkNode_t* tail = bufferPtr->tailPtr;
        EbLinkNode_t* tail_prev = bufferPtr->tailPtr;
        for (unsigned int i = 0; i < bufferPtr->bufferTotalCount; i++) {
            tail = tail->nextPtr;
            tail_prev = tail_prev->prevPtr;
        }
        assert(tail == bufferPtr->tailPtr);
        assert(tail_prev == bufferPtr->tailPtr);

        while (tail != bufferPtr->headPtr) {
            assert(tail->ptr == EB_NULL);
            tail = tail->nextPtr;
        }

    }

    // Increment the Current Count
    ++bufferPtr->currentCount;

    return return_error;
}

/**************************************
 * EbCircularBufferPushFront
 **************************************/
static EB_ERRORTYPE EbCircularBufferPushFront(
    EbCircularBuffer_t   *bufferPtr,
    EB_PTR                objectPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    // Decrement the headIndex
    bufferPtr->headPtr = bufferPtr->headPtr->prevPtr;

    // Copy the pointer into the array
    bufferPtr->headPtr->ptr = objectPtr;

    // Increment the Current Count
    ++bufferPtr->currentCount;
    
    return return_error;
}

/**************************************
 * EbMuxingQueueCtor
 **************************************/
static EB_ERRORTYPE EbMuxingQueueCtor(
    EbMuxingQueue_t   **queueDblPtr,
    EB_U32              objectTotalCount,
    EB_U32              processTotalCount,
    EbFifo_t         ***processFifoPtrArrayPtr)
{
    EbMuxingQueue_t *queuePtr;
    EB_U32 processIndex;
    EB_ERRORTYPE     return_error = EB_ErrorNone;

    EB_MALLOC(EbMuxingQueue_t *, queuePtr, sizeof(EbMuxingQueue_t), EB_N_PTR);
    *queueDblPtr = queuePtr;

    queuePtr->processTotalCount = processTotalCount;

    // Lockout Mutex
    EB_CREATEMUTEX(EB_HANDLE, queuePtr->lockoutMutex, sizeof(EB_HANDLE), EB_MUTEX);

    // Construct Object Circular Buffer
    return_error = EbCircularBufferCtor(
        &queuePtr->objectQueue,
        objectTotalCount);
    if (return_error == EB_ErrorInsufficientResources){
        return EB_ErrorInsufficientResources;
    }
    // Construct Process Circular Buffer
    return_error = EbCircularBufferCtor(
        &queuePtr->processQueue,
        queuePtr->processTotalCount);
    if (return_error == EB_ErrorInsufficientResources){
        return EB_ErrorInsufficientResources;
    }
    // Construct the Process Fifos
    EB_MALLOC(EbFifo_t**, queuePtr->processFifoPtrArray, sizeof(EbFifo_t*) * queuePtr->processTotalCount, EB_N_PTR);

    for(processIndex=0; processIndex < queuePtr->processTotalCount; ++processIndex) {
        EB_MALLOC(EbFifo_t*, queuePtr->processFifoPtrArray[processIndex], sizeof(EbFifo_t), EB_N_PTR);
        return_error = EbFifoCtor(
            queuePtr->processFifoPtrArray[processIndex],
            0,
            objectTotalCount,
            (EbObjectWrapper_t *)EB_NULL,
            (EbObjectWrapper_t *)EB_NULL,
            queuePtr);
        if (return_error == EB_ErrorInsufficientResources){
            return EB_ErrorInsufficientResources;
        }
    }

    *processFifoPtrArrayPtr = queuePtr->processFifoPtrArray;

    return return_error;
}

/**************************************
 * EbMuxingQueueAssignation
 **************************************/
static EB_ERRORTYPE EbMuxingQueueAssignation(
    EbMuxingQueue_t *queuePtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;
    EbFifo_t *processFifoPtr;
    EbObjectWrapper_t *wrapperPtr;

    // while loop
    while((EbCircularBufferEmptyCheck(queuePtr->objectQueue)  == EB_FALSE) &&
            (EbCircularBufferEmptyCheck(queuePtr->processQueue) == EB_FALSE)) {
        // Get the next process
        EbCircularBufferPopFront(
            queuePtr->processQueue,
            (void **) &processFifoPtr);

        // Get the next object
        EbCircularBufferPopFront(
            queuePtr->objectQueue,
            (void **) &wrapperPtr);

        // Block on the Process Fifo's Mutex
        EbBlockOnMutex(processFifoPtr->lockoutMutex);

        // Put the object on the fifo
        EbFifoPushBack(
            processFifoPtr,
            wrapperPtr);

        // Release the Process Fifo's Mutex
        EbReleaseMutex(processFifoPtr->lockoutMutex);

        // Post the semaphore
        EbPostSemaphore(processFifoPtr->countingSemaphore);
    }

    return return_error;
}

/**************************************
 * EbMuxingQueueObjectPushBack
 **************************************/
static EB_ERRORTYPE EbMuxingQueueObjectPushBack(
    EbMuxingQueue_t    *queuePtr,
    EbObjectWrapper_t  *objectPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    //EbCircularBufferPushBack(
    EbCircularBufferRankInsert(
        queuePtr->objectQueue,
        objectPtr);

    EbMuxingQueueAssignation(queuePtr);

    return return_error;
}

/**************************************
* EbMuxingQueueObjectPushFront
**************************************/
static EB_ERRORTYPE EbMuxingQueueObjectPushFront(
	EbMuxingQueue_t    *queuePtr,
	EbObjectWrapper_t  *objectPtr)
{
	EB_ERRORTYPE return_error = EB_ErrorNone;

	EbCircularBufferPushFront(
		queuePtr->objectQueue,
		objectPtr);

	EbMuxingQueueAssignation(queuePtr);

	return return_error;
}

/*********************************************************************
 * EbObjectReleaseEnable
 *   Enables the releaseEnable member of EbObjectWrapper.  Used by
 *   certain objects (e.g. SequenceControlSet) to control whether
 *   EbObjectWrappers are allowed to be released or not.
 *
 *   resourcePtr
 *      Pointer to the SystemResource that manages the EbObjectWrapper.
 *      The emptyFifo's lockoutMutex is used to write protect the
 *      modification of the EbObjectWrapper.
 *
 *   wrapperPtr
 *      Pointer to the EbObjectWrapper to be modified.
 *********************************************************************/
EB_ERRORTYPE EbObjectReleaseEnable(
    EbObjectWrapper_t   *wrapperPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    EbBlockOnMutex(wrapperPtr->systemResourcePtr->emptyQueue->lockoutMutex);

    wrapperPtr->releaseEnable = EB_TRUE;

    EbReleaseMutex(wrapperPtr->systemResourcePtr->emptyQueue->lockoutMutex);

    return return_error;
}

/*********************************************************************
 * EbObjectReleaseDisable
 *   Disables the releaseEnable member of EbObjectWrapper.  Used by
 *   certain objects (e.g. SequenceControlSet) to control whether
 *   EbObjectWrappers are allowed to be released or not.
 *
 *   resourcePtr
 *      Pointer to the SystemResource that manages the EbObjectWrapper.
 *      The emptyFifo's lockoutMutex is used to write protect the
 *      modification of the EbObjectWrapper.
 *
 *   wrapperPtr
 *      Pointer to the EbObjectWrapper to be modified.
 *********************************************************************/
EB_ERRORTYPE EbObjectReleaseDisable(
    EbObjectWrapper_t   *wrapperPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    EbBlockOnMutex(wrapperPtr->systemResourcePtr->emptyQueue->lockoutMutex);

    wrapperPtr->releaseEnable = EB_FALSE;

    EbReleaseMutex(wrapperPtr->systemResourcePtr->emptyQueue->lockoutMutex);

    return return_error;
}

/*********************************************************************
 * EbObjectIncLiveCount
 *   Increments the liveCount member of EbObjectWrapper.  Used by
 *   certain objects (e.g. SequenceControlSet) to count the number of active
 *   pointers of a EbObjectWrapper in pipeline at any point in time.
 *
 *   resourcePtr
 *      Pointer to the SystemResource that manages the EbObjectWrapper.
 *      The emptyFifo's lockoutMutex is used to write protect the
 *      modification of the EbObjectWrapper.
 *
 *   wrapperPtr
 *      Pointer to the EbObjectWrapper to be modified.
 *********************************************************************/
EB_ERRORTYPE EbObjectIncLiveCount(
    EbObjectWrapper_t   *wrapperPtr,
    EB_U32               incrementNumber)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    EbBlockOnMutex(wrapperPtr->systemResourcePtr->emptyQueue->lockoutMutex);

    wrapperPtr->liveCount += incrementNumber;

    EbReleaseMutex(wrapperPtr->systemResourcePtr->emptyQueue->lockoutMutex);

    return return_error;
}

/*********************************************************************
 * EbSystemResourceCtor
 *   Constructor for EbSystemResource.  Fully constructs all members
 *   of EbSystemResource including the object with the passed
 *   ObjectCtor function.
 *
 *   resourcePtr
 *     Pointer that will contain the SystemResource to be constructed.
 *
 *   objectTotalCount
 *     Number of objects to be managed by the SystemResource.
 *
 *   fullFifoEnabled
 *     Bool that describes if the SystemResource is to have an output
 *     fifo.  An outputFifo is not used by certain objects (e.g.
 *     SequenceControlSet).
 *
 *   ObjectCtor
 *     Function pointer to the constructor of the object managed by
 *     SystemResource referenced by resourcePtr. No object level
 *     construction is performed if ObjectCtor is NULL.
 *
 *   objectInitDataPtr

 *     Pointer to data block to be used during the construction of
 *     the object. objectInitDataPtr is passed to ObjectCtor when
 *     ObjectCtor is called.
 *********************************************************************/
EB_ERRORTYPE EbSystemResourceCtor(
    EbSystemResource_t **resourceDblPtr,
    EB_U32               objectTotalCount,
    EB_U32               producerProcessTotalCount,
    EB_U32               consumerProcessTotalCount,
    EbFifo_t          ***producerFifoPtrArrayPtr,
    EbFifo_t          ***consumerFifoPtrArrayPtr,
    EB_BOOL              fullFifoEnabled,
    EB_CTOR              ObjectCtor,
    EB_PTR               objectInitDataPtr)
{
    EB_U32 wrapperIndex;
    EB_ERRORTYPE return_error = EB_ErrorNone;
    // Allocate the System Resource
    EbSystemResource_t *resourcePtr;

    EB_MALLOC(EbSystemResource_t*, resourcePtr, sizeof(EbSystemResource_t), EB_N_PTR);
    *resourceDblPtr = resourcePtr;

    resourcePtr->objectTotalCount = objectTotalCount;

    // Allocate array for wrapper pointers
    EB_MALLOC(EbObjectWrapper_t**, resourcePtr->wrapperPtrPool, sizeof(EbObjectWrapper_t*) * resourcePtr->objectTotalCount, EB_N_PTR);

    // Initialize each wrapper
    for (wrapperIndex=0; wrapperIndex < resourcePtr->objectTotalCount; ++wrapperIndex) {
        EB_MALLOC(EbObjectWrapper_t*, resourcePtr->wrapperPtrPool[wrapperIndex], sizeof(EbObjectWrapper_t), EB_N_PTR);
        resourcePtr->wrapperPtrPool[wrapperIndex]->liveCount            = 0;
        resourcePtr->wrapperPtrPool[wrapperIndex]->releaseEnable        = EB_TRUE;
        resourcePtr->wrapperPtrPool[wrapperIndex]->systemResourcePtr    = resourcePtr;
        resourcePtr->wrapperPtrPool[wrapperIndex]->rank                 = 0;

        // Call the Constructor for each element
        if(ObjectCtor) {
            return_error = ObjectCtor(
                &resourcePtr->wrapperPtrPool[wrapperIndex]->objectPtr,
                objectInitDataPtr);
            if (return_error == EB_ErrorInsufficientResources){
                return EB_ErrorInsufficientResources;
            }
        }
    }

    // Initialize the Empty Queue
    return_error = EbMuxingQueueCtor(
        &resourcePtr->emptyQueue,
        resourcePtr->objectTotalCount,
        producerProcessTotalCount,
        producerFifoPtrArrayPtr);
    if (return_error == EB_ErrorInsufficientResources){
        return EB_ErrorInsufficientResources;
    }
    // Fill the Empty Fifo with every ObjectWrapper
    for (wrapperIndex=0; wrapperIndex < resourcePtr->objectTotalCount; ++wrapperIndex) {
        EbMuxingQueueObjectPushBack(
            resourcePtr->emptyQueue,
            resourcePtr->wrapperPtrPool[wrapperIndex]);
    }

    // Initialize the Full Queue
    if (fullFifoEnabled == EB_TRUE) {
        return_error = EbMuxingQueueCtor(
            &resourcePtr->fullQueue,
            resourcePtr->objectTotalCount,
            consumerProcessTotalCount,
            consumerFifoPtrArrayPtr);
        if (return_error == EB_ErrorInsufficientResources){
            return EB_ErrorInsufficientResources;
        }
    } else {
        resourcePtr->fullQueue  = (EbMuxingQueue_t *)EB_NULL;
        consumerFifoPtrArrayPtr = (EbFifo_t ***)EB_NULL;
    }

    return return_error;
}



/*********************************************************************
 * EbSystemResourceReleaseProcess
 *********************************************************************/
static EB_ERRORTYPE EbReleaseProcess(
    EbFifo_t   *processFifoPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    EbBlockOnMutex(processFifoPtr->queuePtr->lockoutMutex);

    EbCircularBufferPushFront(
        processFifoPtr->queuePtr->processQueue,
        processFifoPtr);

    EbMuxingQueueAssignation(processFifoPtr->queuePtr);

    EbReleaseMutex(processFifoPtr->queuePtr->lockoutMutex);

    return return_error;
}

/*********************************************************************
 * EbSystemResourcePostObject
 *   Queues a full EbObjectWrapper to the SystemResource. This
 *   function posts the SystemResource fullFifo countingSemaphore.
 *   This function is write protected by the SystemResource fullFifo
 *   lockoutMutex.
 *
 *   resourcePtr
 *      Pointer to the SystemResource that the EbObjectWrapper is
 *      posted to.
 *
 *   wrapperPtr
 *      Pointer to EbObjectWrapper to be posted.
 *********************************************************************/
EB_ERRORTYPE EbPostFullObject(
    EbObjectWrapper_t   *objectPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    EbBlockOnMutex(objectPtr->systemResourcePtr->fullQueue->lockoutMutex);

    EbMuxingQueueObjectPushBack(
        objectPtr->systemResourcePtr->fullQueue,
        objectPtr);

    EbReleaseMutex(objectPtr->systemResourcePtr->fullQueue->lockoutMutex);

    return return_error;
}

/*********************************************************************
 * EbSystemResourceReleaseObject
 *   Queues an empty EbObjectWrapper to the SystemResource. This
 *   function posts the SystemResource emptyFifo countingSemaphore.
 *   This function is write protected by the SystemResource emptyFifo
 *   lockoutMutex.
 *
 *   objectPtr
 *      Pointer to EbObjectWrapper to be released.
 *********************************************************************/
EB_ERRORTYPE EbReleaseObject(
    EbObjectWrapper_t   *objectPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    EbBlockOnMutex(objectPtr->systemResourcePtr->emptyQueue->lockoutMutex);

    // Decrement liveCount
    objectPtr->liveCount = (objectPtr->liveCount == 0) ? objectPtr->liveCount : objectPtr->liveCount - 1;

    if((objectPtr->releaseEnable == EB_TRUE) && (objectPtr->liveCount == 0)) {

        // Set liveCount to EB_ObjectWrapperReleasedValue
        objectPtr->liveCount = EB_ObjectWrapperReleasedValue;

		EbMuxingQueueObjectPushFront(
			objectPtr->systemResourcePtr->emptyQueue,
			objectPtr);

    }

    EbReleaseMutex(objectPtr->systemResourcePtr->emptyQueue->lockoutMutex);

    return return_error;
}

/*********************************************************************
 * EbSystemResourceGetEmptyObject
 *   Dequeues an empty EbObjectWrapper from the SystemResource.  This
 *   function blocks on the SystemResource emptyFifo countingSemaphore.
 *   This function is write protected by the SystemResource emptyFifo
 *   lockoutMutex.
 *
 *   resourcePtr
 *      Pointer to the SystemResource that provides the empty
 *      EbObjectWrapper.
 *
 *   wrapperDblPtr
 *      Double pointer used to pass the pointer to the empty
 *      EbObjectWrapper pointer.
 *********************************************************************/
EB_ERRORTYPE EbGetEmptyObject(
    EbFifo_t   *emptyFifoPtr,
    EbObjectWrapper_t **wrapperDblPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    // Queue the Fifo requesting the empty fifo
    EbReleaseProcess(emptyFifoPtr);

    // Block on the counting Semaphore until an empty buffer is available
    EbBlockOnSemaphore(emptyFifoPtr->countingSemaphore);

    // Acquire lockout Mutex
    EbBlockOnMutex(emptyFifoPtr->lockoutMutex);

    // Get the empty object
    EbFifoPopFront(
        emptyFifoPtr,
        wrapperDblPtr);

    // Reset the wrapper's liveCount
    (*wrapperDblPtr)->liveCount = 0;

    // Object release enable
    (*wrapperDblPtr)->releaseEnable = EB_TRUE;

    // Release Mutex
    EbReleaseMutex(emptyFifoPtr->lockoutMutex);

    return return_error;
}

/*********************************************************************
 * EbSystemResourceGetFullObject
 *   Dequeues an full EbObjectWrapper from the SystemResource. This
 *   function blocks on the SystemResource fullFifo countingSemaphore.
 *   This function is write protected by the SystemResource fullFifo
 *   lockoutMutex.
 *
 *   resourcePtr
 *      Pointer to the SystemResource that provides the full
 *      EbObjectWrapper.
 *
 *   wrapperDblPtr
 *      Double pointer used to pass the pointer to the full
 *      EbObjectWrapper pointer.
 *********************************************************************/
EB_ERRORTYPE EbGetFullObject(
    EbFifo_t   *fullFifoPtr,
    EbObjectWrapper_t **wrapperDblPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;

    long start = 0;
    long duration = 0;
    if (fullFifoPtr->dbg_info) {
        start = EbGetSysTimeMs();
    }

    // Queue the process requesting the full fifo
    EbReleaseProcess(fullFifoPtr);

    // Block on the counting Semaphore until an empty buffer is available
    EbBlockOnSemaphore(fullFifoPtr->countingSemaphore);

    // Acquire lockout Mutex
    EbBlockOnMutex(fullFifoPtr->lockoutMutex);

    EbFifoPopFront(
        fullFifoPtr,
        wrapperDblPtr);

    // Release Mutex
    EbReleaseMutex(fullFifoPtr->lockoutMutex);
    if (fullFifoPtr->dbg_info) {
        duration = EbGetSysTimeMs() - start;
        fullFifoPtr->dbg_info->total_wait_time_ms += duration;
        fullFifoPtr->dbg_info->total_wait_counts++;
        if (duration > fullFifoPtr->dbg_info->max_wait_time_ms) {
            fullFifoPtr->dbg_info->max_wait_time_ms = duration;
        }
    }

    return return_error;
}

/******************************************************************************
* EbSystemResourceGetFullObject
*   Dequeues an full EbObjectWrapper from the SystemResource. This
*   function does not block on the SystemResource fullFifo countingSemaphore.
*   This function is write protected by the SystemResource fullFifo
*   lockoutMutex.
*
*   resourcePtr
*      Pointer to the SystemResource that provides the full
*      EbObjectWrapper.
*
*   wrapperDblPtr
*      Double pointer used to pass the pointer to the full
*      EbObjectWrapper pointer.
*******************************************************************************/
EB_ERRORTYPE EbGetFullObjectNonBlocking(
    EbFifo_t   *fullFifoPtr,
    EbObjectWrapper_t **wrapperDblPtr)
{
    EB_ERRORTYPE return_error = EB_ErrorNone;
    EB_BOOL      fifoEmpty;
    // Queue the Fifo requesting the full fifo
    EbReleaseProcess(fullFifoPtr);

    // Acquire lockout Mutex
    EbBlockOnMutex(fullFifoPtr->lockoutMutex);

    fifoEmpty = EbFifoPeakFront(
                        fullFifoPtr);

    // Release Mutex
    EbReleaseMutex(fullFifoPtr->lockoutMutex);

    if (fifoEmpty == EB_FALSE)
        EbGetFullObject(
            fullFifoPtr,
            wrapperDblPtr);
    else
        *wrapperDblPtr = (EbObjectWrapper_t*)EB_NULL;

    return return_error;
}
