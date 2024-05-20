#pragma once

#include <memory.h>

typedef struct ringbuf_t
{
    char* buf;          //buffer with size of at least sizeElements*elementSize
    int sizeElements;   //slot count
    int elementSize;    //one element size in bytes
    int startIdx;       //first filled slot
    int endIdx;         //first nonfilled slot / +1 to last filled slot
    bool isEmpty;       //if startIdx == endIdx ringbuf is either full or empty
} ringbuf_t;

static void ringbuf_init(ringbuf_t* ptr, void* buf, int sizeElements, int elementSize)
{
    ptr->buf = (char*)buf;
    ptr->sizeElements = sizeElements;
    ptr->elementSize = elementSize;

    ptr->startIdx = ptr->endIdx = 0;
    ptr->isEmpty = true;
}

static inline int ringbuf_free_slots(ringbuf_t* ptr)
{
    if (ptr->isEmpty)
        return ptr->sizeElements;

    if (ptr->startIdx == ptr->endIdx)
        return 0;

    return ptr->startIdx > ptr->endIdx
        ? ptr->startIdx - ptr->endIdx
        : ptr->sizeElements - (ptr->endIdx - ptr->startIdx);
}

static inline int ringbuf_filled_slots(ringbuf_t* ptr)
{
    return ptr->sizeElements - ringbuf_free_slots(ptr);
}

static inline bool ringbuf_is_empty(ringbuf_t* ptr)
{
    return ptr->isEmpty;
}

static inline bool ringbuf_is_full(ringbuf_t* ptr)
{
    return !ptr->isEmpty && ptr->startIdx == ptr->endIdx;
}

static inline void ringbuf_clear(ringbuf_t* ptr)
{
    ptr->startIdx = ptr->endIdx = 0;
    ptr->isEmpty = true;
}

static int ringbuf_put(ringbuf_t* ptr, const void* buf, int elementCount)
{
    if (ringbuf_is_full(ptr))
        return 0;

    int freeSlots = ringbuf_free_slots(ptr);
    int ret = freeSlots > elementCount
        ? elementCount
        : freeSlots;

    int elementsBeforeWrap = ptr->sizeElements - ptr->endIdx;

    if (elementsBeforeWrap >= ret)
        memcpy(ptr->buf + ptr->endIdx * ptr->elementSize, buf, ret * ptr->elementSize);
    else
    {
        memcpy(ptr->buf + ptr->endIdx * ptr->elementSize, buf, elementsBeforeWrap * ptr->elementSize);
        memcpy(ptr->buf,
            (const char*)buf + elementsBeforeWrap * ptr->elementSize,
            (ret - elementsBeforeWrap) * ptr->elementSize);
    }

    ptr->endIdx += ret;
    ptr->isEmpty = false;

    if (ptr->endIdx >= ptr->sizeElements)
        ptr->endIdx -= ptr->sizeElements;

    return ret;
}

static bool ringbuf_put_one(ringbuf_t* ptr, const void* element)
{
    if (ringbuf_is_full(ptr))
        return false;

    for (int i = 0; i < ptr->elementSize; ++i)
        ptr->buf[ptr->endIdx * ptr->elementSize + i] = ((const char*)element)[i];

    ++ptr->endIdx;
    ptr->isEmpty = false;

    if (ptr->endIdx == ptr->sizeElements)
        ptr->endIdx = 0;

    return true;
}

static int ringbuf_get(ringbuf_t* ptr, void* buf, int elementCount)
{
    if (ringbuf_is_empty(ptr))
        return 0;

    int filledSlots = ringbuf_filled_slots(ptr);
    int ret = filledSlots > elementCount
        ? elementCount
        : filledSlots;

    int elementsBeforeWrap = ptr->sizeElements - ptr->startIdx;

    if (elementsBeforeWrap >= ret)
        memcpy(buf, ptr->buf + ptr->startIdx * ptr->elementSize, ret * ptr->elementSize);
    else
    {
        memcpy(buf, ptr->buf + ptr->startIdx * ptr->elementSize, elementsBeforeWrap * ptr->elementSize);
        memcpy((char*)buf + elementsBeforeWrap * ptr->elementSize,
            ptr->buf,
            (ret - elementsBeforeWrap) * ptr->elementSize);
    }

    ptr->startIdx += ret;

    if (ptr->startIdx >= ptr->sizeElements)
        ptr->startIdx -= ptr->sizeElements;

    if (ptr->startIdx == ptr->endIdx)
        ptr->isEmpty = true;

    return ret;
}


static bool ringbuf_get_one(ringbuf_t* ptr, void* element)
{
    if (ringbuf_is_empty(ptr))
        return false;

    for (int i = 0; i < ptr->elementSize; ++i)
        ((char*)element)[i] = ptr->buf[ptr->startIdx * ptr->elementSize + i];

    ++ptr->startIdx;

    if (ptr->startIdx == ptr->sizeElements)
        ptr->startIdx = 0;

    if (ptr->startIdx == ptr->endIdx)
        ptr->isEmpty = true;

    return true;
}