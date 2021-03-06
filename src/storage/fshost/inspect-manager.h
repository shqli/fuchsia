// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FSHOST_INSPECT_MANAGER_H_
#define SRC_STORAGE_FSHOST_INSPECT_MANAGER_H_

#include <lib/inspect/cpp/inspector.h>

#include <queue>

#include <fs/pseudo_dir.h>

namespace devmgr {

// Utility to open a directory at the given `path` under `root`. The resulting channel handle will
// be in `result`. The returned `status` indicates whether the operation was successful or not.
zx_status_t OpenNode(zx::unowned_channel root, const std::string& path, uint32_t mode,
                     zx::channel* result);

// Management of fshost inspect data.
class InspectManager {
 public:
  InspectManager();
  ~InspectManager();

  // Returns the diagnostics directory where inspect data is contained.
  fbl::RefPtr<fs::PseudoDir> Initialize(async_dispatcher* dispatcher);

  // Creates a lazy node which serves stats about the given path.
  void ServeStats(const std::string& path, fbl::RefPtr<fs::Vnode> root);

  const inspect::Inspector& inspector() const { return inspector_; }

 private:
  inspect::Inspector inspector_;

  // Fills information about the size of files and directories under the given `root` under the
  // given `node` and emplaces it in the given `inspector`. Returns the total size of `root`.
  void FillFileTreeSizes(zx::channel root, inspect::Node node, inspect::Inspector* inspector);

  // Queries the filesystem about stats of the given `root` and stores them in the given `inspector`
  void FillStats(zx::unowned_channel root, inspect::Inspector* inspector);
};

// A directory entry returned by `DirectoryEntriesIterator`
struct DirectoryEntry {
  // The name of the entry.
  std::string name;
  // A handle to the node this entry represents.
  zx::channel node;
  // If the entry its a file, this contains the content size. If the entry is a directory, this will
  // be zero.
  size_t size;
  // Whether the entry is a directory or not.
  bool is_dir;
};

// Utility to lazily iterate over the entries of a directory.
class DirectoryEntriesIterator {
 public:
  // Create a new lazy iterator.
  DirectoryEntriesIterator(zx::channel directory);

  // Get the next entry. If there's no more entries left (it finished), returns std::nullopt
  // forever.
  std::optional<DirectoryEntry> GetNext();

  bool finished() { return finished_; }

 private:
  // The directory which entries will be retrieved.
  zx::channel directory_;
  // Pending entries to return.
  std::queue<std::string> pending_entries_;
  // Whether or not the iterator has finished.
  bool finished_;

  // Creates a `DirectoryEntry`. If it fails to retrieve the entry `entry_name` attributes,
  // returns `std::nullopt`.
  std::optional<DirectoryEntry> MaybeMakeEntry(const std::string& entry_name);

  // Reads the next set of dirents and loads them into `pending_entries_`.
  void RefreshPendingEntries();
};

}  // namespace devmgr

#endif  // SRC_STORAGE_FSHOST_INSPECT_MANAGER_H_
