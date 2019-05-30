/*
* Copyright(c) 2018 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#include <stdlib.h>
#include <assert.h>
#include "EbEntropyCodingProcess.h"
#include "EbTransforms.h"
#include "EbEncDecResults.h"
#include "EbEntropyCodingResults.h"
#include "EbRateControlTasks.h"

/******************************************************
 * Enc Dec Context Constructor
 ******************************************************/
EB_ERRORTYPE EntropyCodingContextCtor(
    EntropyCodingContext_t **contextDblPtr,
    EbFifo_t                *encDecInputFifoPtr,
    EbFifo_t                *packetizationOutputFifoPtr,
    EbFifo_t                *rateControlOutputFifoPtr,
    EB_BOOL                  is16bit)
{
    EntropyCodingContext_t *contextPtr;
    EB_MALLOC(EntropyCodingContext_t*, contextPtr, sizeof(EntropyCodingContext_t), EB_N_PTR);
    *contextDblPtr = contextPtr;

    contextPtr->is16bit = is16bit;

    // Input/Output System Resource Manager FIFOs
    contextPtr->encDecInputFifoPtr          = encDecInputFifoPtr;
    contextPtr->entropyCodingOutputFifoPtr  = packetizationOutputFifoPtr;
    contextPtr->rateControlOutputFifoPtr    = rateControlOutputFifoPtr;

    return EB_ErrorNone;
}

/***********************************************
 * Entropy Coding Reset Neighbor Arrays
 ***********************************************/
static void EntropyCodingResetNeighborArrays(PictureControlSet_t *pictureControlSetPtr, EB_U16 tileIdx)
{
    NeighborArrayUnitReset(pictureControlSetPtr->modeTypeNeighborArray[tileIdx]);
    NeighborArrayUnitReset(pictureControlSetPtr->leafDepthNeighborArray[tileIdx]);
    NeighborArrayUnitReset(pictureControlSetPtr->intraLumaModeNeighborArray[tileIdx]);
    NeighborArrayUnitReset(pictureControlSetPtr->skipFlagNeighborArray[tileIdx]);

    return;
}

/**************************************************
 * Reset Entropy Coding Picture
 **************************************************/
static void ResetEntropyCodingPicture(
	EntropyCodingContext_t  *contextPtr,
	PictureControlSet_t     *pictureControlSetPtr,
	SequenceControlSet_t    *sequenceControlSetPtr)
{
    EB_U32 tileCnt = pictureControlSetPtr->tileRowCount * pictureControlSetPtr->tileColumnCount;
    EB_U32 tileIdx = 0;

    for (tileIdx = 0; tileIdx < tileCnt; tileIdx++) {
        ResetBitstream(EntropyCoderGetBitstreamPtr(pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCoderPtr));
    }

	EB_U32                       entropyCodingQp;

	contextPtr->is16bit = (EB_BOOL)(sequenceControlSetPtr->staticConfig.encoderBitDepth > EB_8BIT);

	// SAO
	pictureControlSetPtr->saoFlag[0] = EB_TRUE;
	pictureControlSetPtr->saoFlag[1] = EB_TRUE;

	// QP
	contextPtr->qp = pictureControlSetPtr->pictureQp;
	// Asuming cb and cr offset to be the same for chroma QP in both slice and pps for lambda computation

	EB_U8	qpScaled = CLIP3((EB_S8)MIN_QP_VALUE, (EB_S8)MAX_CHROMA_MAP_QP_VALUE, (EB_S8)(contextPtr->qp + pictureControlSetPtr->cbQpOffset + pictureControlSetPtr->sliceCbQpOffset));
	contextPtr->chromaQp = MapChromaQp(qpScaled);


	if (pictureControlSetPtr->useDeltaQp) {
		entropyCodingQp = pictureControlSetPtr->pictureQp;
	}
	else {
		entropyCodingQp = pictureControlSetPtr->pictureQp;
	}


	// Reset CABAC Contexts
	// Reset QP Assignement
	pictureControlSetPtr->prevCodedQp = pictureControlSetPtr->pictureQp;
	pictureControlSetPtr->prevQuantGroupCodedQp = pictureControlSetPtr->pictureQp;

    for (tileIdx = 0; tileIdx < tileCnt; tileIdx++) {
        ResetEntropyCoder(
                sequenceControlSetPtr->encodeContextPtr,
                pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCoderPtr,
                entropyCodingQp,
                pictureControlSetPtr->sliceType);
	
        EntropyCodingResetNeighborArrays(pictureControlSetPtr, tileIdx);
    }


    return;
}


/******************************************************
 * EncDec Configure LCU
 ******************************************************/
static void EntropyCodingConfigureLcu(
    EntropyCodingContext_t  *contextPtr,
    LargestCodingUnit_t     *lcuPtr,
    PictureControlSet_t     *pictureControlSetPtr)
{
    contextPtr->qp = pictureControlSetPtr->pictureQp;

	// Asuming cb and cr offset to be the same for chroma QP in both slice and pps for lambda computation

	EB_U8	qpScaled = CLIP3((EB_S8)MIN_QP_VALUE, (EB_S8)MAX_CHROMA_MAP_QP_VALUE, (EB_S8)(contextPtr->qp + pictureControlSetPtr->cbQpOffset + pictureControlSetPtr->sliceCbQpOffset));
	contextPtr->chromaQp = MapChromaQp(qpScaled);

    lcuPtr->qp = contextPtr->qp;

    return;
}

/******************************************************
 * Entropy Coding Lcu
 ******************************************************/
static void EntropyCodingLcu(
    LargestCodingUnit_t               *lcuPtr,
    PictureControlSet_t               *pictureControlSetPtr,
    SequenceControlSet_t              *sequenceControlSetPtr,
    EB_U32                             lcuOriginX,
    EB_U32                             lcuOriginY,
    EB_BOOL                            terminateSliceFlag,
    EB_U16                             tileIdx,
    EB_U32                             pictureOriginX,
    EB_U32                             pictureOriginY)
{

	EbPictureBufferDesc_t *coeffPicturePtr = lcuPtr->quantizedCoeff;

    //rate Control
    EB_U32                       writtenBitsBeforeQuantizedCoeff;
    EB_U32                       writtenBitsAfterQuantizedCoeff;
    EntropyCoder_t               *entropyCoderPtr = pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCoderPtr;
    //store the number of written bits before coding quantized coeffs (flush is not called yet): 
    // The total number of bits is 
    // number of written bits
    // + 32  - bits remaining in interval Low Value
    // + number of buffered byte * 8
    // This should be only for coeffs not any flag
    writtenBitsBeforeQuantizedCoeff =  ((OutputBitstreamUnit_t*)EntropyCoderGetBitstreamPtr(entropyCoderPtr))->writtenBitsCount +
                                        32 - ((CabacEncodeContext_t*) entropyCoderPtr->cabacEncodeContextPtr)->bacEncContext.bitsRemainingNum +
                                        (((CabacEncodeContext_t*)entropyCoderPtr->cabacEncodeContextPtr)->bacEncContext.tempBufferedBytesNum <<3);

    if(sequenceControlSetPtr->staticConfig.enableSaoFlag && (pictureControlSetPtr->saoFlag[0] || pictureControlSetPtr->saoFlag[1])) {

        // Code SAO parameters
        EncodeLcuSaoParameters(
            lcuPtr,
            entropyCoderPtr,
            pictureControlSetPtr->saoFlag[0],
            pictureControlSetPtr->saoFlag[1],
            (EB_U8)sequenceControlSetPtr->staticConfig.encoderBitDepth);
    }

    EncodeLcu(
        lcuPtr,
        lcuOriginX,
        lcuOriginY,
        pictureControlSetPtr,
        sequenceControlSetPtr->lcuSize,
        entropyCoderPtr,
        coeffPicturePtr,
        pictureControlSetPtr->modeTypeNeighborArray[tileIdx],
        pictureControlSetPtr->leafDepthNeighborArray[tileIdx],
        pictureControlSetPtr->intraLumaModeNeighborArray[tileIdx],
        pictureControlSetPtr->skipFlagNeighborArray[tileIdx],
		pictureOriginX,
		pictureOriginY);

    //Jing:TODO
    // extend the totalBits to tile, for tile based brc
    
    //store the number of written bits after coding quantized coeffs (flush is not called yet): 
    // The total number of bits is 
    // number of written bits
    // + 32  - bits remaining in interval Low Value
    // + number of buffered byte * 8
    writtenBitsAfterQuantizedCoeff =   ((OutputBitstreamUnit_t*)EntropyCoderGetBitstreamPtr(entropyCoderPtr))->writtenBitsCount +
                                        32 - ((CabacEncodeContext_t*) entropyCoderPtr->cabacEncodeContextPtr)->bacEncContext.bitsRemainingNum +
                                        (((CabacEncodeContext_t*) entropyCoderPtr->cabacEncodeContextPtr)->bacEncContext.tempBufferedBytesNum <<3);

    lcuPtr->totalBits = writtenBitsAfterQuantizedCoeff - writtenBitsBeforeQuantizedCoeff;

    pictureControlSetPtr->ParentPcsPtr->quantizedCoeffNumBits += lcuPtr->quantizedCoeffsBits;

    /*********************************************************
    *Note - At the end of each LCU, HEVC adds 1 bit to indicate that
    if the current LCU is the end of a slice, where 0x1 means it is the
    end of slice and 0x0 means not. Currently we assume that each slice
    contains integer number of tiles, thus the trailing 1 bit for the
    non-ending LCU of a tile is always 0x0 and the trailing 1 bit for the
    tile ending LCU can be 0x0 or 0x1. In the entropy coding process, we
    can not decide the slice boundary so we can not write the trailing 1 bit
    for the tile ending LCU.
    *********************************************************/
    EncodeTerminateLcu(
        entropyCoderPtr,
        terminateSliceFlag);

    return;
}

/******************************************************
 * Update Entropy Coding Rows 
 *
 * This function is responsible for synchronizing the
 *   processing of Entropy Coding LCU-rows and starts 
 *   processing of LCU-rows as soon as their inputs are 
 *   available and the previous LCU-row has completed.  
 *   At any given time, only one segment row per picture
 *   is being processed.
 *
 * The function has two parts:
 *
 * (1) Update the available row index which tracks
 *   which LCU Row-inputs are available.
 *
 * (2) Increment the lcu-row counter as the segment-rows
 *   are completed.
 *
 * Since there is the potentential for thread collusion,
 *   a MUTEX a used to protect the sensitive data and
 *   the execution flow is separated into two paths
 *
 * (A) Initial update.
 *  -Update the Completion Mask [see (1) above]
 *  -If the picture is not currently being processed,
 *     check to see if the next segment-row is available
 *     and start processing.
 * (B) Continued processing
 *  -Upon the completion of a segment-row, check
 *     to see if the next segment-row's inputs have
 *     become available and begin processing if so.
 *
 * On last important point is that the thread-safe
 *   code section is kept minimally short. The MUTEX
 *   should NOT be locked for the entire processing
 *   of the segment-row (B) as this would block other
 *   threads from performing an update (A).
 ******************************************************/
static EB_BOOL UpdateEntropyCodingRows(
    PictureControlSet_t *pictureControlSetPtr,
    EB_U32              *rowIndex,
    EB_U32               rowCount,
    EB_U32               tileIdx,
    EB_BOOL             *initialProcessCall)
{
    EB_BOOL processNextRow = EB_FALSE;

    EntropyTileInfo *infoPtr = pictureControlSetPtr->entropyCodingInfo[tileIdx];
    // Note, any writes & reads to status variables (e.g. inProgress) in MD-CTRL must be thread-safe
    EbBlockOnMutex(infoPtr->entropyCodingMutex);

    // Update availability mask
    if (*initialProcessCall == EB_TRUE) {
        unsigned i;

        for(i=*rowIndex; i < *rowIndex + rowCount; ++i) {
            infoPtr->entropyCodingRowArray[i] = EB_TRUE;
        }
        
        while(infoPtr->entropyCodingRowArray[infoPtr->entropyCodingCurrentAvailableRow] == EB_TRUE &&
              infoPtr->entropyCodingCurrentAvailableRow < infoPtr->entropyCodingRowCount)
        {
            ++infoPtr->entropyCodingCurrentAvailableRow;
        }
    }

    // Release inProgress token
    if(*initialProcessCall == EB_FALSE && infoPtr->entropyCodingInProgress == EB_TRUE) {
        infoPtr->entropyCodingInProgress = EB_FALSE;
    }

    // Test if the picture is not already complete AND not currently being worked on by another ENCDEC process
    if(infoPtr->entropyCodingCurrentRow < infoPtr->entropyCodingRowCount && 
       infoPtr->entropyCodingRowArray[infoPtr->entropyCodingCurrentRow] == EB_TRUE &&
       infoPtr->entropyCodingInProgress == EB_FALSE)
    {
        // Test if the next LCU-row is ready to go
        if(infoPtr->entropyCodingCurrentRow <= infoPtr->entropyCodingCurrentAvailableRow)
        {
            infoPtr->entropyCodingInProgress = EB_TRUE;
            *rowIndex = infoPtr->entropyCodingCurrentRow++;
            processNextRow = EB_TRUE;
        }
    }

    *initialProcessCall = EB_FALSE;

    EbReleaseMutex(infoPtr->entropyCodingMutex);

    return processNextRow;
}
#if TILES

//static EB_BOOL WaitForAllEntropyCodingRows(
//    PictureControlSet_t *pictureControlSetPtr,
//    EB_U32               rowIndex,
//    EB_U32               rowCount,
//    EB_U32               tileIdx)
//{
//    EB_BOOL pic_ready = EB_TRUE;
//    unsigned i;
//    EB_U32 tileCnt = pictureControlSetPtr->tileRowCount * pictureControlSetPtr->tileColumnCount;
//
//    EntropyTileInfo *infoPtr = pictureControlSetPtr->entropyCodingInfo[tileIdx];
//    // Note, any writes & reads to status variables (e.g. inProgress) in MD-CTRL must be thread-safe
//    EbBlockOnMutex(infoPtr->entropyCodingMutex);
//
//    //printf("--POC %d, Tile %d, (%d, %d)\n", pictureControlSetPtr->pictureNumber, tileIdx, rowIndex, rowIndex+rowCount);
//
//    //Jing: TODO: Check if all entropy row within this tile is ready
//    for (i = rowIndex; i < rowIndex + rowCount; ++i) {
//        infoPtr->entropyCodingRowArray[i] = EB_TRUE;
//    }
//
//    while (infoPtr->entropyCodingRowArray[infoPtr->entropyCodingCurrentAvailableRow] == EB_TRUE &&
//            infoPtr->entropyCodingCurrentAvailableRow < infoPtr->entropyCodingRowCount)
//    {
//        ++infoPtr->entropyCodingCurrentAvailableRow;
//    }
//
//
//    // Test if all rows are available
//    for (i = 0; i < (unsigned)infoPtr->entropyCodingRowCount; ++i) {
//        pic_ready = (infoPtr->entropyCodingRowArray[i] == EB_FALSE) ? EB_FALSE : pic_ready;
//    }
//    infoPtr->entropyCodingPicDone = pic_ready;
//    /////////////
//
//
//    EbReleaseMutex(infoPtr->entropyCodingMutex);
//
//    if (pic_ready) {
//        // Jing:Check if all tiles are ready
//        //      Add a picture level mutex protection
//        EbBlockOnMutex(pictureControlSetPtr->entropyCodingPicMutex);
//        for (i = 0; i < tileCnt; i++) {
//            if (pictureControlSetPtr->entropyCodingInfo[i]->entropyCodingPicDone == EB_FALSE) {
//                pic_ready = EB_FALSE;
//                //printf("current POC %d not fully ready, tile %d missing\n", pictureControlSetPtr->pictureNumber, i);
//                break;
//            }
//        }
//        EbReleaseMutex(pictureControlSetPtr->entropyCodingPicMutex);
//    }
//    if (pic_ready) {
//        //printf("POC %d all tiles ready\n", pictureControlSetPtr->pictureNumber);
//    }
//    return pic_ready;
//}

/**************************************************
 * Reset Entropy Coding Tiles
 **************************************************/
//static void ResetEntropyCodingTile(
//    EntropyCodingContext_t  *contextPtr,
//    PictureControlSet_t     *pictureControlSetPtr,
//    SequenceControlSet_t    *sequenceControlSetPtr)
//{
//    EB_U32                       entropyCodingQp;
//
//    contextPtr->is16bit = (EB_BOOL)(sequenceControlSetPtr->staticConfig.encoderBitDepth > EB_8BIT);
//
//    // SAO
//    pictureControlSetPtr->saoFlag[0] = EB_TRUE;
//    pictureControlSetPtr->saoFlag[1] = EB_TRUE;
//
//    // QP
//    contextPtr->qp = pictureControlSetPtr->pictureQp;
//    contextPtr->chromaQp = MapChromaQp(contextPtr->qp);
//
//    if (pictureControlSetPtr->useDeltaQp) {
//        entropyCodingQp = pictureControlSetPtr->pictureQp;
//    }
//    else {
//        entropyCodingQp = pictureControlSetPtr->pictureQp;
//    }
//
//    // Reset QP Assignement
//    pictureControlSetPtr->prevCodedQp = pictureControlSetPtr->pictureQp;
//    pictureControlSetPtr->prevQuantGroupCodedQp = pictureControlSetPtr->pictureQp;
//
//    // Reset CABAC Contexts
//    ResetEntropyCoder(
//            sequenceControlSetPtr->encodeContextPtr,
//            pictureControlSetPtr->entropyCoderPtr,
//            entropyCodingQp,
//            pictureControlSetPtr->sliceType);
//
//    // Jing: extend to tiles
//    EntropyCodingResetNeighborArrays(pictureControlSetPtr);
//
//    return;
//}
#endif
/******************************************************
 * Entropy Coding Kernel
 ******************************************************/
void* EntropyCodingKernel(void *inputPtr)
{
    // Context & SCS & PCS
    EntropyCodingContext_t                  *contextPtr = (EntropyCodingContext_t*) inputPtr;
    PictureControlSet_t                     *pictureControlSetPtr;
    SequenceControlSet_t                    *sequenceControlSetPtr;

    // Input
    EbObjectWrapper_t                       *encDecResultsWrapperPtr;
    EncDecResults_t                         *encDecResultsPtr;

    // Output
    EbObjectWrapper_t                       *entropyCodingResultsWrapperPtr;
    EntropyCodingResults_t                  *entropyCodingResultsPtr;

    // LCU Loop variables
    LargestCodingUnit_t                     *lcuPtr;
    EB_U16                                   lcuIndex;
    EB_U8                                    lcuSize;
    EB_U8                                    lcuSizeLog2;
    EB_U32                                   xLcuIndex;
    EB_U32                                   yLcuIndex;
    EB_U32                                   lcuOriginX;
    EB_U32                                   lcuOriginY;
    EB_BOOL                                  lastLcuFlagInSlice;
    EB_BOOL                                  lastLcuFlagInTile;
    EB_U32                                   pictureWidthInLcu;
    EB_U32                                   tileWidthInLcu;
    EB_U32                                   tileHeightInLcu;
    // Variables
    EB_BOOL                                  initialProcessCall;
    EB_U32                                   tileIdx;
    EB_U32                                   tileCnt;
    EB_U32                                   xLcuStart;
    EB_U32                                   yLcuStart;

    for(;;) {

        // Get Mode Decision Results
        EbGetFullObject(
            contextPtr->encDecInputFifoPtr,
            &encDecResultsWrapperPtr);
        encDecResultsPtr       = (EncDecResults_t*) encDecResultsWrapperPtr->objectPtr;
        pictureControlSetPtr   = (PictureControlSet_t*) encDecResultsPtr->pictureControlSetWrapperPtr->objectPtr;
        sequenceControlSetPtr  = (SequenceControlSet_t*) pictureControlSetPtr->sequenceControlSetWrapperPtr->objectPtr;
        tileIdx                = encDecResultsPtr->tileIndex;
        lastLcuFlagInSlice     = EB_FALSE;
        lastLcuFlagInTile      = EB_FALSE;
        tileCnt                = pictureControlSetPtr->tileRowCount * pictureControlSetPtr->tileColumnCount;
#if DEADLOCK_DEBUG
        SVT_LOG("POC %lld EC IN \n", pictureControlSetPtr->pictureNumber);
#endif
        //SVT_LOG("[%lld]: POC %lld EC IN, tile %d, (%d, %d) \n",
        //        EbGetSysTimeMs(),
        //        pictureControlSetPtr->pictureNumber, tileIdx,
        //        encDecResultsPtr->completedLcuRowIndexStart,
        //        encDecResultsPtr->completedLcuRowIndexStart + encDecResultsPtr->completedLcuRowCount);
        // LCU Constants
        lcuSize     = sequenceControlSetPtr->lcuSize;
        lcuSizeLog2 = (EB_U8)Log2f(lcuSize);
        contextPtr->lcuSize = lcuSize;
        pictureWidthInLcu = (sequenceControlSetPtr->lumaWidth + lcuSize - 1) >> lcuSizeLog2;
        tileWidthInLcu = sequenceControlSetPtr->tileColumnArray[tileIdx % pictureControlSetPtr->tileColumnCount];
        tileHeightInLcu = sequenceControlSetPtr->tileRowArray[tileIdx / pictureControlSetPtr->tileColumnCount];
        xLcuStart = yLcuStart = 0;
        for (unsigned int i = 0; i < (tileIdx % pictureControlSetPtr->tileColumnCount); i++) {
            xLcuStart += sequenceControlSetPtr->tileColumnArray[i];
        }
        for (unsigned int i = 0; i < (tileIdx / pictureControlSetPtr->tileColumnCount); i++) {
            yLcuStart += sequenceControlSetPtr->tileRowArray[i];
        }
        //printf("TileIdx %d, LCU start (%d, %d)\n", tileIdx, xLcuStart, yLcuStart);

        if (tileCnt == 1) {
            assert(pictureWidthInLcu == tileWidthInLcu);
        }

        if (tileCnt >= 1) {
            //assert(tileIdx == 0);
            initialProcessCall = EB_TRUE;
            yLcuIndex = encDecResultsPtr->completedLcuRowIndexStart;   
            
            // LCU-loops
            while(UpdateEntropyCodingRows(pictureControlSetPtr, &yLcuIndex, encDecResultsPtr->completedLcuRowCount, tileIdx, &initialProcessCall) == EB_TRUE) 
            {
                EB_U32 rowTotalBits = 0;

                if(yLcuIndex == 0) {
                    EbBlockOnMutex(pictureControlSetPtr->entropyCodingPicMutex);
                    if (pictureControlSetPtr->entropyCodingPicResetFlag) {
                        //printf("[%lld]:Reset pic %d at tile %d\n",
                        //        EbGetSysTimeMs(),
                        //        pictureControlSetPtr->pictureNumber, tileIdx);
                        pictureControlSetPtr->entropyCodingPicResetFlag = EB_FALSE;
                        ResetEntropyCodingPicture(
                                contextPtr, 
                                pictureControlSetPtr,
                                sequenceControlSetPtr);
                    }
                    EbReleaseMutex(pictureControlSetPtr->entropyCodingPicMutex);
					pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCodingPicDone = EB_FALSE;
                }

                for(xLcuIndex = 0; xLcuIndex < tileWidthInLcu; ++xLcuIndex) {
                    lcuIndex = (EB_U16)((xLcuIndex + xLcuStart) + (yLcuIndex + yLcuStart) * pictureWidthInLcu);
                    lcuPtr = pictureControlSetPtr->lcuPtrArray[lcuIndex];

                    lcuOriginX = (xLcuIndex + xLcuStart) << lcuSizeLog2;
                    lcuOriginY = (yLcuIndex + yLcuStart) << lcuSizeLog2;
                    //Jing:TODO:
                    //Check for lastLcu, since tiles are parallelized, last LCU may not the the last one in slice
                    lastLcuFlagInSlice = (lcuIndex == pictureControlSetPtr->lcuTotalCount - 1) ? EB_TRUE : EB_FALSE;
                    lastLcuFlagInTile = (xLcuIndex == tileWidthInLcu - 1 && yLcuIndex == tileHeightInLcu - 1) ? EB_TRUE : EB_FALSE;
                    if (sequenceControlSetPtr->tileSliceMode) {
                        lastLcuFlagInSlice = lastLcuFlagInTile;
                    }
            
                    // Configure the LCU
                    EntropyCodingConfigureLcu(
                        contextPtr,
                        lcuPtr,
                        pictureControlSetPtr);
            
                    // Entropy Coding
                    EntropyCodingLcu(
                        lcuPtr,
                        pictureControlSetPtr,
                        sequenceControlSetPtr,
                        lcuOriginX,
                        lcuOriginY,
                        lastLcuFlagInSlice,
                        tileIdx,
                        (xLcuIndex + xLcuStart) * lcuSize,
                        (yLcuIndex + yLcuStart) * lcuSize);

                    rowTotalBits += lcuPtr->totalBits;
                }

                // At the end of each LCU-row, send the updated bit-count to Entropy Coding
                {
                    //Jing: TODO
                    //this is per tile, brc can use it or just ignore it
                    EbObjectWrapper_t *rateControlTaskWrapperPtr;
                    RateControlTasks_t *rateControlTaskPtr;

                    // Get Empty EncDec Results
                    EbGetEmptyObject(
                        contextPtr->rateControlOutputFifoPtr,
                        &rateControlTaskWrapperPtr);
                    rateControlTaskPtr = (RateControlTasks_t*) rateControlTaskWrapperPtr->objectPtr;
                    rateControlTaskPtr->taskType = RC_ENTROPY_CODING_ROW_FEEDBACK_RESULT;
                    rateControlTaskPtr->pictureNumber = pictureControlSetPtr->pictureNumber;
                    rateControlTaskPtr->tileIndex = tileIdx;
                    rateControlTaskPtr->rowNumber = yLcuIndex; //Jing: yLcuIndex within tile
                    rateControlTaskPtr->bitCount = rowTotalBits;

                    rateControlTaskPtr->pictureControlSetWrapperPtr = 0;
                    rateControlTaskPtr->segmentIndex = ~0u;
                    
                    // Post EncDec Results
                    EbPostFullObject(rateControlTaskWrapperPtr);
                }

				EbBlockOnMutex(pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCodingMutex);
				if (pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCodingPicDone == EB_FALSE) {
                    //Jing: TODO
                    //Double check here for slice/tile, currently bitstream has a whole slice
                    //Maybe have to store the av(e) part for different tiles and copy it as a whole to slice bitstream

					// If the picture is complete, terminate the slice
					if (pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCodingCurrentRow == pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCodingRowCount)
					{
						EB_U32 refIdx;
                        EB_BOOL pic_ready = EB_TRUE;

                        assert(lastLcuFlagInTile == EB_TRUE);

                        //Jing:tile end, may not be the slice end
                        if (!lastLcuFlagInSlice) {
                            //printf("[%lld]:Encode tile end for tile %d\n", EbGetSysTimeMs(), tileIdx);
                            EncodeTileFinish(pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCoderPtr);
                        } else {
                            //printf("[%lld]:Encode slice end for tile %d\n", EbGetSysTimeMs(), tileIdx);
						    EncodeSliceFinish(pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCoderPtr);
                        }

                        //Jing: TODO
                        //Release the ref if the whole pic are done
                        EbBlockOnMutex(pictureControlSetPtr->entropyCodingPicMutex);
						pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCodingPicDone = EB_TRUE;
                        for (EB_U32 i = 0; i < tileCnt; i++) {
                            if (pictureControlSetPtr->entropyCodingInfo[i]->entropyCodingPicDone == EB_FALSE) {
                                pic_ready = EB_FALSE;
                                //printf("current POC %d not fully ready, tile %d missing\n", pictureControlSetPtr->pictureNumber, i);
                                break;
                            }
                        }
                        EbReleaseMutex(pictureControlSetPtr->entropyCodingPicMutex);

                        if (pic_ready) {
                            // Release the List 0 Reference Pictures
                            for (refIdx = 0; refIdx < pictureControlSetPtr->ParentPcsPtr->refList0Count; ++refIdx) {
                                if (pictureControlSetPtr->refPicPtrArray[0] != EB_NULL) {

                                    EbReleaseObject(pictureControlSetPtr->refPicPtrArray[0]);
                                }
                            }

                            // Release the List 1 Reference Pictures
                            for (refIdx = 0; refIdx < pictureControlSetPtr->ParentPcsPtr->refList1Count; ++refIdx) {
                                if (pictureControlSetPtr->refPicPtrArray[1] != EB_NULL) {

                                    EbReleaseObject(pictureControlSetPtr->refPicPtrArray[1]);
                                }
                            }

                            // Get Empty Entropy Coding Results
                            EbGetEmptyObject(
                                    contextPtr->entropyCodingOutputFifoPtr,
                                    &entropyCodingResultsWrapperPtr);
                            entropyCodingResultsPtr = (EntropyCodingResults_t*)entropyCodingResultsWrapperPtr->objectPtr;
                            entropyCodingResultsPtr->pictureControlSetWrapperPtr = encDecResultsPtr->pictureControlSetWrapperPtr;

                            printf("[%lld]: Entropy post result, POC %d\n", EbGetSysTimeMs(), pictureControlSetPtr->pictureNumber);
                            // Post EntropyCoding Results
                            EbPostFullObject(entropyCodingResultsWrapperPtr);
                        }
					} // End if(PictureCompleteFlag)
				}
				EbReleaseMutex(pictureControlSetPtr->entropyCodingInfo[tileIdx]->entropyCodingMutex);
			}
        }
#if 0//TILES
        else
        {
            unsigned xLcuStart, yLcuStart, rowIndex, columnIndex;

            yLcuIndex = encDecResultsPtr->completedLcuRowIndexStart;

            // Jing: TODO: wait for a whole picture for simplicity
            // Update EC-rows, exit if picture is "in-progress" 
            if (WaitForAllEntropyCodingRows(pictureControlSetPtr, yLcuIndex, encDecResultsPtr->completedLcuRowCount, tileIdx) == EB_TRUE) {
                //printf("EntropyCoding start to work on POC %d... \n", pictureControlSetPtr->pictureNumber);
                ResetEntropyCodingPicture(
                        contextPtr,
                        pictureControlSetPtr,
                        sequenceControlSetPtr);

                // Tile-loops
                yLcuStart = 0;
                for (rowIndex = 0; rowIndex < sequenceControlSetPtr->tileRowCount; ++rowIndex)
                {
                    xLcuStart = 0;
                    for (columnIndex = 0; columnIndex < sequenceControlSetPtr->tileColumnCount; ++columnIndex)
                    {
                        //printf("Reset tile, Index %d, poc %d\n",
                        //        columnIndex + rowIndex * sequenceControlSetPtr->tileColumnCount,
                        //        pictureControlSetPtr->pictureNumber);
                        ResetEntropyCodingTile(
                                contextPtr,
                                pictureControlSetPtr,
                                sequenceControlSetPtr);

                        if (sequenceControlSetPtr->tileSliceMode == 1 && (xLcuStart !=0 || yLcuStart != 0)) {
                            CabacEncodeContext_t *cabacEncodeCtxPtr = (CabacEncodeContext_t*)pictureControlSetPtr->entropyCoderPtr->cabacEncodeContextPtr;
                            EncodeSliceHeader(
                                    xLcuStart + yLcuStart * pictureWidthInLcu,
                                    pictureControlSetPtr->pictureQp,
                                    pictureControlSetPtr,
                                    (OutputBitstreamUnit_t*) &(cabacEncodeCtxPtr->bacEncContext.m_pcTComBitIf));
                        }

                        // LCU-loops
                        for (yLcuIndex = yLcuStart; yLcuIndex < yLcuStart + sequenceControlSetPtr->tileRowArray[rowIndex]; ++yLcuIndex)
                        {
                            for (xLcuIndex = xLcuStart; xLcuIndex < xLcuStart + sequenceControlSetPtr->tileColumnArray[columnIndex]; ++xLcuIndex)
                            {
                                lcuIndex = xLcuIndex + yLcuIndex * pictureWidthInLcu;
                                lcuPtr = pictureControlSetPtr->lcuPtrArray[lcuIndex];
                                lcuOriginX = xLcuIndex << lcuSizeLog2;
                                lcuOriginY = yLcuIndex << lcuSizeLog2;
                                if (sequenceControlSetPtr->tileSliceMode == 0) {
                                    lastLcuFlag = (lcuIndex == pictureControlSetPtr->lcuTotalCount - 1) ? EB_TRUE : EB_FALSE;
                                } else {
                                    lastLcuFlag = (xLcuIndex == (xLcuStart + sequenceControlSetPtr->tileColumnArray[columnIndex] -1) && yLcuIndex == (yLcuStart + sequenceControlSetPtr->tileRowArray[rowIndex] -1)) ? EB_TRUE : EB_FALSE;
                                }

                                // Configure the LCU
                                EntropyCodingConfigureLcu(
                                        contextPtr,
                                        lcuPtr,
                                        pictureControlSetPtr);

                                // Entropy Coding
                                EntropyCodingLcu(
                                        lcuPtr,
                                        pictureControlSetPtr,
                                        sequenceControlSetPtr,
                                        lcuOriginX,
                                        lcuOriginY,
                                        lastLcuFlag,
                                        xLcuIndex * lcuSize,
                                        yLcuIndex * lcuSize);
                            }
                        }

                        if (sequenceControlSetPtr->tileSliceMode == 0 && lastLcuFlag == EB_FALSE) {
                            EncodeTileFinish(pictureControlSetPtr->entropyCoderPtr);
                        } else if (sequenceControlSetPtr->tileSliceMode) {
                            EncodeSliceFinish(pictureControlSetPtr->entropyCoderPtr);
                        }

                        xLcuStart += sequenceControlSetPtr->tileColumnArray[columnIndex];
                    }
                    yLcuStart += sequenceControlSetPtr->tileRowArray[rowIndex];
                }

                assert(lastLcuFlag == EB_TRUE);
                // If the picture is complete, terminate the slice, 2nd pass DLF, SAO application
                if (lastLcuFlag == EB_TRUE) {
                    EB_U32 refIdx;

                    if (sequenceControlSetPtr->tileSliceMode == 0) {
                        EncodeSliceFinish(pictureControlSetPtr->entropyCoderPtr);
                    }

                    // Release the List 0 Reference Pictures
                    for (refIdx = 0; refIdx < pictureControlSetPtr->ParentPcsPtr->refList0Count; ++refIdx) {
                        if (pictureControlSetPtr->refPicPtrArray[0]/*[refIdx]*/ != EB_NULL) {
                            EbReleaseObject(pictureControlSetPtr->refPicPtrArray[0]/*[refIdx]*/);
                        }
                    }

                    // Release the List 1 Reference Pictures
                    for (refIdx = 0; refIdx < pictureControlSetPtr->ParentPcsPtr->refList1Count; ++refIdx) {
                        if (pictureControlSetPtr->refPicPtrArray[1]/*[refIdx]*/ != EB_NULL) {
                            EbReleaseObject(pictureControlSetPtr->refPicPtrArray[1]/*[refIdx]*/);
                        }
                    }

                    // Get Empty Entropy Coding Results
                    EbGetEmptyObject(
                            contextPtr->entropyCodingOutputFifoPtr,
                            &entropyCodingResultsWrapperPtr);
                    entropyCodingResultsPtr = (EntropyCodingResults_t*)entropyCodingResultsWrapperPtr->objectPtr;
                    entropyCodingResultsPtr->pictureControlSetWrapperPtr = encDecResultsPtr->pictureControlSetWrapperPtr;

                    //printf("----Posting POC %d to packetization kernel\n", pictureControlSetPtr->pictureNumber);
                    printf("[%lld]: Entropy post result\n", EbGetSysTimeMs());
                    // Post EntropyCoding Results
                    EbPostFullObject(entropyCodingResultsWrapperPtr);

                } // End if(PictureCompleteFlag)
            }
        }
#endif


#if DEADLOCK_DEBUG
        SVT_LOG("POC %lld EC OUT \n", pictureControlSetPtr->pictureNumber);
#endif
        // Release Mode Decision Results
        EbReleaseObject(encDecResultsWrapperPtr);

    }
    
    return EB_NULL;
}
