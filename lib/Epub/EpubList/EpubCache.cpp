#include "EpubCache.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef UNIT_TEST
#include <esp_log.h>
#else
#define ESP_LOGI(tag, args...)
#define ESP_LOGW(tag, args...)
#define ESP_LOGE(tag, args...)
#endif

namespace
{
const char *TAG = "EPUB_CACHE";
const uint8_t CACHE_MAGIC[8] = {'A', '1', '4', 'E', 'P', 'C', '0', '2'};
const size_t CACHE_HEADER_SIZE = 16;
const size_t MAX_CACHE_SIZE = 512 * 1024;
const uint16_t MAX_CACHE_BOOKS = 64;
const uint16_t MAX_CACHE_CHAPTERS = 4096;

uint32_t crc32(const uint8_t *data, size_t length)
{
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++)
  {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++)
    {
      crc = (crc >> 1) ^ (0xEDB88320 & -(int32_t)(crc & 1));
    }
  }
  return ~crc;
}

template <typename T>
void append_number(std::vector<uint8_t> &output, T value)
{
  for (size_t i = 0; i < sizeof(T); i++)
  {
    output.push_back((uint8_t)(((uint64_t)value >> (i * 8)) & 0xFF));
  }
}

void append_string(std::vector<uint8_t> &output, const std::string &value)
{
  const uint16_t length = std::min((size_t)UINT16_MAX, value.size());
  append_number(output, length);
  output.insert(output.end(), value.begin(), value.begin() + length);
}

template <typename T>
bool read_number(const std::vector<uint8_t> &input, size_t &offset, T &value)
{
  if (offset + sizeof(T) > input.size())
  {
    return false;
  }
  uint64_t result = 0;
  for (size_t i = 0; i < sizeof(T); i++)
  {
    result |= (uint64_t)input[offset++] << (i * 8);
  }
  value = (T)result;
  return true;
}

bool read_string(const std::vector<uint8_t> &input, size_t &offset,
                 std::string &value, size_t maximum_length)
{
  uint16_t length = 0;
  if (!read_number(input, offset, length) || length > maximum_length ||
      offset + length > input.size())
  {
    return false;
  }
  value.assign((const char *)&input[offset], length);
  offset += length;
  return true;
}
}

EpubCache::EpubCache(const std::string &path)
    : cache_path(path), temp_path(path + ".tmp")
{
}

bool EpubCache::read_file(const std::string &path, std::vector<CachedBook> &result) const
{
  FILE *file = fopen(path.c_str(), "rb");
  if (!file)
  {
    return false;
  }
  fseek(file, 0, SEEK_END);
  const long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (file_size < (long)CACHE_HEADER_SIZE || file_size > (long)MAX_CACHE_SIZE)
  {
    fclose(file);
    return false;
  }

  std::vector<uint8_t> data(file_size);
  const bool read_ok = fread(data.data(), 1, data.size(), file) == data.size();
  fclose(file);
  if (!read_ok || memcmp(data.data(), CACHE_MAGIC, sizeof(CACHE_MAGIC)) != 0)
  {
    return false;
  }

  size_t header_offset = sizeof(CACHE_MAGIC);
  uint32_t payload_size = 0;
  uint32_t expected_crc = 0;
  if (!read_number(data, header_offset, payload_size) ||
      !read_number(data, header_offset, expected_crc) ||
      payload_size != data.size() - CACHE_HEADER_SIZE ||
      crc32(data.data() + CACHE_HEADER_SIZE, payload_size) != expected_crc)
  {
    return false;
  }

  size_t offset = CACHE_HEADER_SIZE;
  uint16_t book_count = 0;
  if (!read_number(data, offset, book_count) || book_count > MAX_CACHE_BOOKS)
  {
    return false;
  }

  std::vector<CachedBook> parsed;
  parsed.reserve(book_count);
  for (uint16_t book_index = 0; book_index < book_count; book_index++)
  {
    CachedBook book = {};
    uint8_t toc_cached = 0;
    uint8_t resume_cached = 0;
    uint16_t chapter_count = 0;
    if (!read_string(data, offset, book.path, 1024) ||
        !read_string(data, offset, book.title, 4096) ||
        !read_number(data, offset, book.size) ||
        !read_number(data, offset, book.mtime) ||
        !read_number(data, offset, toc_cached) ||
        !read_number(data, offset, resume_cached) ||
        !read_number(data, offset, book.resume_section) ||
        !read_number(data, offset, book.resume_page) ||
        !read_number(data, offset, chapter_count) ||
        chapter_count > MAX_CACHE_CHAPTERS)
    {
      return false;
    }
    book.toc_cached = toc_cached != 0;
    book.resume_cached = resume_cached != 0;
    book.seen = false;
    book.chapters.reserve(chapter_count);
    for (uint16_t chapter_index = 0; chapter_index < chapter_count; chapter_index++)
    {
      CachedChapter chapter;
      if (!read_string(data, offset, chapter.title, 4096) ||
          !read_number(data, offset, chapter.spine_index))
      {
        return false;
      }
      book.chapters.push_back(chapter);
    }
    parsed.push_back(book);
  }
  if (offset != data.size())
  {
    return false;
  }
  result.swap(parsed);
  return true;
}

bool EpubCache::load()
{
  std::vector<CachedBook> loaded;
  if (read_file(cache_path, loaded))
  {
    books.swap(loaded);
    ESP_LOGI(TAG, "Loaded %d cached books", books.size());
    return true;
  }
  if (read_file(temp_path, loaded))
  {
    books.swap(loaded);
    dirty = true;
    ESP_LOGW(TAG, "Recovered %d cached books from temporary index", books.size());
    return true;
  }
  ESP_LOGI(TAG, "No valid persistent index");
  return false;
}

CachedBook &EpubCache::upsert(const std::string &path, uint64_t size, int64_t mtime)
{
  for (auto &book : books)
  {
    if (book.path == path)
    {
      if (book.size != size || book.mtime != mtime)
      {
        book.size = size;
        book.mtime = mtime;
        book.title.clear();
        book.chapters.clear();
        book.toc_cached = false;
        book.resume_cached = false;
        book.resume_section = 0;
        book.resume_page = 0;
        dirty = true;
      }
      book.seen = true;
      return book;
    }
  }
  CachedBook book = {};
  book.path = path;
  book.size = size;
  book.mtime = mtime;
  book.toc_cached = false;
  book.resume_cached = false;
  book.seen = true;
  books.push_back(book);
  dirty = true;
  return books.back();
}

const CachedBook *EpubCache::find(const std::string &path, uint64_t size, int64_t mtime) const
{
  for (const auto &book : books)
  {
    if (book.path == path && book.size == size && book.mtime == mtime)
    {
      return &book;
    }
  }
  return nullptr;
}

void EpubCache::begin_scan()
{
  for (auto &book : books)
  {
    book.seen = false;
  }
}

void EpubCache::finish_scan()
{
  const size_t previous_size = books.size();
  books.erase(std::remove_if(books.begin(), books.end(),
                             [](const CachedBook &book) { return !book.seen; }),
              books.end());
  dirty = dirty || books.size() != previous_size;
}

void EpubCache::store_title(const std::string &path, uint64_t size, int64_t mtime,
                            const std::string &title)
{
  CachedBook &book = upsert(path, size, mtime);
  if (book.title != title)
  {
    book.title = title;
    dirty = true;
  }
}

bool EpubCache::get_chapters(const std::string &path, uint64_t size, int64_t mtime,
                             std::vector<CachedChapter> &chapters) const
{
  const CachedBook *book = find(path, size, mtime);
  if (!book || !book->toc_cached)
  {
    return false;
  }
  chapters = book->chapters;
  return true;
}

bool EpubCache::get_resume(const std::string &path, uint16_t &section, uint16_t &page) const
{
  for (const auto &book : books)
  {
    if (book.path == path && book.resume_cached)
    {
      section = book.resume_section;
      page = book.resume_page;
      return true;
    }
  }
  return false;
}

void EpubCache::store_resume(const std::string &path, uint16_t section, uint16_t page)
{
  for (auto &book : books)
  {
    if (book.path == path)
    {
      if (!book.resume_cached || book.resume_section != section || book.resume_page != page)
      {
        book.resume_cached = true;
        book.resume_section = section;
        book.resume_page = page;
        dirty = true;
      }
      return;
    }
  }
}

void EpubCache::store_chapters(const std::string &path, uint64_t size, int64_t mtime,
                               const std::string &title,
                               const std::vector<CachedChapter> &chapters)
{
  CachedBook &book = upsert(path, size, mtime);
  book.title = title;
  book.chapters = chapters;
  book.toc_cached = true;
  dirty = true;
}

bool EpubCache::save()
{
  if (!dirty)
  {
    return true;
  }

  std::vector<uint8_t> payload;
  append_number(payload, (uint16_t)books.size());
  for (const auto &book : books)
  {
    append_string(payload, book.path);
    append_string(payload, book.title);
    append_number(payload, book.size);
    append_number(payload, book.mtime);
    append_number(payload, (uint8_t)(book.toc_cached ? 1 : 0));
    append_number(payload, (uint8_t)(book.resume_cached ? 1 : 0));
    append_number(payload, book.resume_section);
    append_number(payload, book.resume_page);
    append_number(payload, (uint16_t)book.chapters.size());
    for (const auto &chapter : book.chapters)
    {
      append_string(payload, chapter.title);
      append_number(payload, chapter.spine_index);
    }
  }

  std::vector<uint8_t> output;
  output.insert(output.end(), CACHE_MAGIC, CACHE_MAGIC + sizeof(CACHE_MAGIC));
  append_number(output, (uint32_t)payload.size());
  append_number(output, crc32(payload.data(), payload.size()));
  output.insert(output.end(), payload.begin(), payload.end());

  FILE *file = fopen(temp_path.c_str(), "wb");
  if (!file)
  {
    ESP_LOGE(TAG, "Could not open temporary index for writing");
    return false;
  }
  const bool write_ok = fwrite(output.data(), 1, output.size(), file) == output.size() &&
                        fflush(file) == 0 && fsync(fileno(file)) == 0;
  fclose(file);
  if (!write_ok)
  {
    ESP_LOGE(TAG, "Could not write temporary index");
    return false;
  }

  std::vector<CachedBook> verified;
  if (!read_file(temp_path, verified))
  {
    ESP_LOGE(TAG, "Temporary index failed validation");
    return false;
  }
  remove(cache_path.c_str());
  if (rename(temp_path.c_str(), cache_path.c_str()) != 0)
  {
    ESP_LOGE(TAG, "Could not activate persistent index");
    return false;
  }
  dirty = false;
  ESP_LOGI(TAG, "Saved %d cached books", books.size());
  return true;
}
