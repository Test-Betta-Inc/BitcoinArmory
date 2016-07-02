#include "FileMap.h"

FileMap::FileMap(BlkFile& blk)
{
   lastSeenCumulated_.store(0, std::memory_order_relaxed);
   fnum_ = blk.fnum;
#ifdef WIN32
   int fd = _open(blk.path.c_str(), _O_RDONLY | _O_BINARY);
   if (fd == -1)
      throw std::runtime_error("failed to open file");

   mapsize_ = blk.filesize;
   filemap_ = (uint8_t*)malloc(mapsize_);
   _read(fd, filemap_, mapsize_);
   _close(fd);
#else
   int fd = open(blk.path.c_str(), O_RDONLY);
   if (fd == -1)
      throw std::runtime_error("failed to open file");

   mapsize_ = blk.filesize;
   filemap_ = (uint8_t*)malloc(mapsize_);
   read(fd, filemap_, mapsize_);

   close(fd);
#endif

   fetch_ = FETCH_FETCHED;
}

////////////////////////////////////////////////////////////////////////////////
FileMap::FileMap(FileMap&& fm)
{
   this->filemap_ = fm.filemap_;
   this->mapsize_ = fm.mapsize_;
   lastSeenCumulated_.store(0, std::memory_order_relaxed);

   fnum_ = fm.fnum_;
   fm.filemap_ = nullptr;
   
   fetch_ = fm.fetch_;
}


////////////////////////////////////////////////////////////////////////////////
FileMap::~FileMap()
{
   if (filemap_ != nullptr)
      free(filemap_);

   filemap_ = nullptr;
}

////////////////////////////////////////////////////////////////////////////////
void FileMap::getRawBlock(BinaryDataRef& bdr, uint64_t offset, uint32_t size,
   std::atomic<uint64_t>& lastSeenCumulative)
{
   bdr.setRef(filemap_ + offset, size);

   lastSeenCumulated_.store(
      lastSeenCumulative.fetch_add(size, std::memory_order_relaxed) + size,
      std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////
///
/// BlockFileAccessor
///
////////////////////////////////////////////////////////////////////////////////
BlockFileAccessor::BlockFileAccessor(shared_ptr<vector<BlkFile>> blkfiles,
   BFA_PREFETCH prefetch)
   : blkFiles_(blkfiles), prefetch_(prefetch)
{
   lastSeenCumulative_.store(0, memory_order_relaxed);

   if (prefetch == PREFETCH_NONE)
      return;

   tID_ = thread(prefetchThread, this);
}

////////////////////////////////////////////////////////////////////////////////
BlockFileAccessor::~BlockFileAccessor()
{
   //make sure to shutdown the prefetch thread before returning from the dtor
   if (!tID_.joinable())
      return;

   {
      unique_lock<mutex> lock(prefetchMu_);
      runThread_ = false;
      prefetchCV_.notify_all();
   }

   tID_.join();
}

////////////////////////////////////////////////////////////////////////////////
void BlockFileAccessor::getRawBlock(BinaryDataRef& bdr, uint32_t fnum,
   uint64_t offset, uint32_t size, FileMapContainer* fmpPtr)
{
   shared_ptr<FileMap>* fmptr = nullptr;
   if (fmpPtr != nullptr &&
      fmpPtr->prev_ != nullptr &&
      *fmpPtr->prev_ != nullptr)
   {
      if ((*(fmpPtr->prev_))->fnum_ == fnum)
         fmptr = fmpPtr->prev_;
   }

   if (fmptr == nullptr)
      fmptr = &getFileMap(fnum);

   (*fmptr)->getRawBlock(bdr, offset, size, lastSeenCumulative_);

   if (fmpPtr != nullptr)
      fmpPtr->current_ = *fmptr;

   //clean up maps that haven't been used for a while
   if (lastSeenCumulative_.load(memory_order_relaxed) >= nextThreshold_)
   {
      unique_lock<mutex> lock(mu_);
      auto mapIter = blkMaps_.begin();

      while (mapIter != blkMaps_.end())
      {
         if (mapIter->second->lastSeenCumulated_ + threshold_ <
            lastSeenCumulative_.load(memory_order_relaxed))
         {
            if (mapIter->second.use_count() == 1)
            {
               blkMaps_.erase(mapIter++);
               continue;
            }
         }

         ++mapIter;
      }

      nextThreshold_ =
         lastSeenCumulative_.load(memory_order_relaxed) + threshold_;
   }
}

////////////////////////////////////////////////////////////////////////////////
shared_ptr<FileMap>& BlockFileAccessor::getFileMap(uint32_t fnum)
{
   unique_lock<mutex> lock(mu_);

   auto mapIter = blkMaps_.find(fnum);
   if (mapIter == blkMaps_.end())
   {
      shared_ptr<FileMap> fm(new FileMap((*blkFiles_)[fnum]));
      auto result = blkMaps_.insert(make_pair(fnum, fm));
      mapIter = result.first;
   }

   if (mapIter->second->fetch_ != FETCH_ACCESSED && 
       prefetch_ != PREFETCH_NONE)
   {
      //signal the prefetch thread to grab the next file
      uint32_t nextFnum = fnum + 1;
      if (prefetch_ == PREFETCH_FORWARD)
      {
         if (nextFnum > blkFiles_->size() - 1)
            nextFnum = UINT32_MAX;
      }
      else
      {
         nextFnum = fnum - 1;
      }

      //We only try to lock the prefetch thread mutex. If it fails, it means
      //another thread is already filling the prefetch queue, or the thread is
      //busy.
      unique_lock<mutex> lock(prefetchMu_, defer_lock);
      if (lock.try_lock())
      {
         prefetchFileNum_ = nextFnum;
         prefetchCV_.notify_all();
      }
   }

   mapIter->second->fetch_ = FETCH_ACCESSED;
   return mapIter->second;
}

////////////////////////////////////////////////////////////////////////////////
void BlockFileAccessor::dropFileMap(uint32_t fnum)
{
   unique_lock<mutex> lock(mu_);
   blkMaps_.erase(fnum);
}

////////////////////////////////////////////////////////////////////////////////
void BlockFileAccessor::prefetchThread(BlockFileAccessor* bfaPtr)
{
   if (bfaPtr == nullptr)
      return;

   unique_lock<mutex> lock(bfaPtr->prefetchMu_);

   while (bfaPtr->runThread_)
   {
      {
         unique_lock<mutex> bfaLock(bfaPtr->mu_);

         if (bfaPtr->prefetchFileNum_ != UINT32_MAX)
         {
            shared_ptr<FileMap> fm(
               new FileMap((*bfaPtr->blkFiles_)[bfaPtr->prefetchFileNum_]));

            bfaPtr->blkMaps_[bfaPtr->prefetchFileNum_] = fm;
         }
      }

      bfaPtr->prefetchCV_.wait(lock);
   }
}


