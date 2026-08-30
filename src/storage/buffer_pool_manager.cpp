#include "storage/buffer_pool_manager.h"

#include <cstring>

namespace dbengine {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager* disk_manager,
                                   LogManager* log_manager)
    : pool_size_(pool_size),
      pages_(new Page[pool_size]),
      disk_manager_(disk_manager),
      log_manager_(log_manager) {
  free_list_.clear();
  for (size_t i = 0; i < pool_size_; ++i) {
    free_list_.push_back(static_cast<frame_id_t>(pool_size_ - 1 - i));
  }
  replacer_ = std::make_unique<ClockReplacer>(pool_size_);
}

BufferPoolManager::~BufferPoolManager() {
  FlushAllPages();
  delete[] pages_;
}

frame_id_t BufferPoolManager::FindReplacementFrame() {
  if (!free_list_.empty()) {
    frame_id_t fid = free_list_.front();
    free_list_.pop_front();
    return fid;
  }
  frame_id_t victim = INVALID_FRAME_ID;
  if (replacer_->Victim(&victim)) return victim;
  return INVALID_FRAME_ID;
}

Page* BufferPoolManager::FetchPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t fid = it->second;
    pages_[fid].IncPinCount();
    replacer_->RecordAccess(fid);
    replacer_->SetEvictable(fid, false);
    return &pages_[fid];
  }

  frame_id_t fid = FindReplacementFrame();
  if (fid == INVALID_FRAME_ID) return nullptr;

  page_id_t old_pid = pages_[fid].GetPageId();
  if (old_pid != INVALID_PAGE_ID && pages_[fid].IsDirty()) {
    if (log_manager_ &&
        pages_[fid].GetPageLSN() > log_manager_->GetFlushedLSN()) {
      Status fs = log_manager_->Flush();
      if (!fs.ok()) return nullptr;
    }
    Status s = disk_manager_->WritePage(old_pid, pages_[fid].GetData());
    if (!s.ok()) return nullptr;
  }
  replacer_->Remove(fid);

  Status s = disk_manager_->ReadPage(page_id, pages_[fid].GetData());
  if (!s.ok()) return nullptr;

  pages_[fid].SetPageId(page_id);
  pages_[fid].SetDirty(false);
  pages_[fid].IncPinCount();
  page_table_[page_id] = fid;
  replacer_->RecordAccess(fid);
  replacer_->SetEvictable(fid, false);
  return &pages_[fid];
}

bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return false;

  Page& p = pages_[it->second];
  if (p.PinCount() <= 0) return false;
  p.DecPinCount();
  if (is_dirty) p.SetDirty(true);

  if (p.PinCount() == 0) {
    replacer_->SetEvictable(it->second, true);
  }
  return true;
}

Status BufferPoolManager::FlushPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return Status::OK();
  Page& p = pages_[it->second];
  if (!p.IsDirty()) return Status::OK();
  if (log_manager_ && p.GetPageLSN() > log_manager_->GetFlushedLSN()) {
    Status fs = log_manager_->Flush();
    if (!fs.ok()) return fs;
  }
  Status s = disk_manager_->WritePage(page_id, p.GetData());
  if (!s.ok()) return s;
  p.SetDirty(false);
  return Status::OK();
}

Status BufferPoolManager::FlushAllPages() {
  std::lock_guard<std::mutex> lock(latch_);
  Status first_error = Status::OK();
  for (const auto& kv : page_table_) {
    page_id_t pid = kv.first;
    frame_id_t fid = kv.second;
    Page& p = pages_[fid];
    if (!p.IsDirty()) continue;
    if (log_manager_ && p.GetPageLSN() > log_manager_->GetFlushedLSN()) {
      Status fs = log_manager_->Flush();
      if (!fs.ok() && first_error.ok()) first_error = fs;
    }
    Status s = disk_manager_->WritePage(pid, p.GetData());
    if (!s.ok() && first_error.ok()) first_error = s;
    else p.SetDirty(false);
  }
  return first_error;
}

Page* BufferPoolManager::NewPage(page_id_t* page_id_out) {
  std::lock_guard<std::mutex> lock(latch_);

  frame_id_t fid = FindReplacementFrame();
  if (fid == INVALID_FRAME_ID) return nullptr;

  page_id_t old_pid = pages_[fid].GetPageId();
  if (old_pid != INVALID_PAGE_ID && pages_[fid].IsDirty()) {
    if (log_manager_ &&
        pages_[fid].GetPageLSN() > log_manager_->GetFlushedLSN()) {
      Status fs = log_manager_->Flush();
      if (!fs.ok()) return nullptr;
    }
    Status s = disk_manager_->WritePage(old_pid, pages_[fid].GetData());
    if (!s.ok()) return nullptr;
  }
  replacer_->Remove(fid);

  page_id_t new_pid = disk_manager_->AllocatePage();
  std::memset(pages_[fid].GetData(), 0, PAGE_SIZE);
  pages_[fid].SetPageId(new_pid);
  pages_[fid].SetDirty(false);
  pages_[fid].SetPageLSN(INVALID_LSN);
  pages_[fid].IncPinCount();
  page_table_[new_pid] = fid;
  replacer_->RecordAccess(fid);
  replacer_->SetEvictable(fid, false);

  *page_id_out = new_pid;
  return &pages_[fid];
}

bool BufferPoolManager::DeletePage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) return true;

  frame_id_t fid = it->second;
  if (pages_[fid].PinCount() > 0) return false;

  page_table_.erase(it);
  replacer_->Remove(fid);
  pages_[fid].SetPageId(INVALID_PAGE_ID);
  pages_[fid].SetDirty(false);
  free_list_.push_back(fid);
  return true;
}

}  // namespace dbengine
