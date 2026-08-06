#include <unity.h>

#include <EpubList/EpubCache.h>

#include <stdio.h>
#include <unistd.h>

void test_epub_cache_round_trip(void)
{
  char cache_path[128];
  snprintf(cache_path, sizeof(cache_path), "/tmp/atomic14-epub-cache-%d.bin", getpid());
  const std::string temp_path = std::string(cache_path) + ".tmp";
  unlink(cache_path);
  unlink(temp_path.c_str());

  {
    EpubCache cache(cache_path);
    cache.begin_scan();
    cache.store_title("/fs/book.epub", 1234, 5678, "Book title");
    std::vector<CachedChapter> chapters = {
        {"Chapter one", 2},
        {"Chapter two", 7},
    };
    cache.store_chapters("/fs/book.epub", 1234, 5678, "Book title", chapters);
    cache.store_resume("/fs/book.epub", 7, 23);
    cache.finish_scan();
    TEST_ASSERT_TRUE(cache.save());
  }

  {
    EpubCache cache(cache_path);
    TEST_ASSERT_TRUE(cache.load());
    const CachedBook *book = cache.find("/fs/book.epub", 1234, 5678);
    TEST_ASSERT_NOT_NULL(book);
    TEST_ASSERT_EQUAL_STRING("Book title", book->title.c_str());

    std::vector<CachedChapter> chapters;
    TEST_ASSERT_TRUE(cache.get_chapters("/fs/book.epub", 1234, 5678, chapters));
    TEST_ASSERT_EQUAL(2, chapters.size());
    TEST_ASSERT_EQUAL_STRING("Chapter two", chapters[1].title.c_str());
    TEST_ASSERT_EQUAL(7, chapters[1].spine_index);

    uint16_t section = 0;
    uint16_t page = 0;
    TEST_ASSERT_TRUE(cache.get_resume("/fs/book.epub", section, page));
    TEST_ASSERT_EQUAL(7, section);
    TEST_ASSERT_EQUAL(23, page);
    TEST_ASSERT_FALSE(cache.get_resume("/fs/missing.epub", section, page));

    TEST_ASSERT_NULL(cache.find("/fs/book.epub", 1235, 5678));
    TEST_ASSERT_FALSE(cache.get_chapters("/fs/book.epub", 1234, 5679, chapters));

    cache.store_title("/fs/book.epub", 1235, 5678, "Revised book");
    TEST_ASSERT_FALSE(cache.get_resume("/fs/book.epub", section, page));
  }

  unlink(cache_path);
  unlink(temp_path.c_str());
}
