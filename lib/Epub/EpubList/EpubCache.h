#pragma once

#include <stdint.h>
#include <string>
#include <vector>

struct CachedChapter
{
  std::string title;
  uint16_t spine_index;
};

struct CachedBook
{
  std::string path;
  std::string title;
  uint64_t size;
  int64_t mtime;
  bool toc_cached;
  bool seen;
  std::vector<CachedChapter> chapters;
};

class EpubCache
{
private:
  std::string cache_path;
  std::string temp_path;
  std::vector<CachedBook> books;
  bool dirty = false;

  CachedBook &upsert(const std::string &path, uint64_t size, int64_t mtime);
  bool read_file(const std::string &path, std::vector<CachedBook> &result) const;

public:
  explicit EpubCache(const std::string &path);
  bool load();
  bool save();
  void begin_scan();
  void finish_scan();
  const CachedBook *find(const std::string &path, uint64_t size, int64_t mtime) const;
  void store_title(const std::string &path, uint64_t size, int64_t mtime,
                   const std::string &title);
  bool get_chapters(const std::string &path, uint64_t size, int64_t mtime,
                    std::vector<CachedChapter> &chapters) const;
  void store_chapters(const std::string &path, uint64_t size, int64_t mtime,
                      const std::string &title,
                      const std::vector<CachedChapter> &chapters);
};
