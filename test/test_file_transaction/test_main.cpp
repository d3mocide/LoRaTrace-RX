// file_transaction.h had no test until the 2026-09-07 audit (A02/A09). Both
// of its jobs are only reachable on hardware otherwise: a settings write
// interrupted by power loss, and a CSV that already exists on the card but
// is empty, truncated, or from another schema. The fake filesystem below
// models exactly the operations SD.h gives us, plus the two failure modes
// that matter — a card that refuses to open a file, and one that accepts an
// open and then writes fewer bytes than asked.

#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "../../src/file_transaction.h"

namespace {

struct FakeFs;

// Mirrors the subset of Arduino's File this header uses. Copyable, because
// `auto f = fs.open(...)` reassigns one.
struct FakeFile {
    FakeFs *fs = nullptr;
    std::string path;
    bool valid = false;
    bool writable = false;
    size_t pos = 0;

    explicit operator bool() const { return valid; }
    size_t size() const;
    int read();
    bool seek(size_t to);
    size_t write(const uint8_t *data, size_t len);
    bool available() const { return pos < size(); }
    void close() { valid = false; }
};

struct FakeFs {
    std::map<std::string, std::string> files;
    // Every write beyond this many bytes is dropped, modelling a full card
    // that still reports a successful open.
    size_t writeCap = SIZE_MAX;
    bool openFails = false;
    std::vector<std::string> opened;

    bool exists(const char *path) { return files.count(path) != 0; }
    bool remove(const char *path) { return files.erase(path) != 0; }
    bool rename(const char *from, const char *to) {
        auto it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(it);
        return true;
    }
    FakeFile open(const char *path, const char *mode) {
        opened.push_back(std::string(mode) + ":" + path);
        FakeFile f;
        f.fs = this;
        f.path = path;
        if (openFails) return f;
        const bool read = mode[0] == 'r';
        if (read && !exists(path)) return f;
        if (mode[0] == 'w') files[path].clear();
        if (!read) files[path]; // "a"/"w" create
        f.valid = true;
        f.writable = !read;
        f.pos = 0;
        return f;
    }
};

size_t FakeFile::size() const { return valid ? fs->files[path].size() : 0; }
int FakeFile::read() {
    if (!valid || pos >= fs->files[path].size()) return -1;
    return (uint8_t)fs->files[path][pos++];
}
bool FakeFile::seek(size_t to) {
    if (!valid || to > fs->files[path].size()) return false;
    pos = to;
    return true;
}
size_t FakeFile::write(const uint8_t *data, size_t len) {
    if (!valid || !writable) return 0;
    std::string &body = fs->files[path];
    size_t wrote = 0;
    for (; wrote < len && body.size() < fs->writeCap; ++wrote) body.push_back((char)data[wrote]);
    return wrote;
}

constexpr const char *HEADER = "a,b,c";
constexpr const char *PATH = "/loratrace/run0001/detections.csv";

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- replaceTextFile / recoverTextFile (A09) -----------------------------

void test_replace_leaves_exactly_one_committed_file() {
    FakeFs fs;
    TEST_ASSERT_TRUE(replaceTextFile(fs, "/s.txt", "one\n", 4));
    TEST_ASSERT_EQUAL_STRING("one\n", fs.files["/s.txt"].c_str());
    TEST_ASSERT_FALSE(fs.exists("/s.txt.tmp"));

    // The second write keeps the previous version as the backup, so there is
    // never a moment with no complete file on the card.
    TEST_ASSERT_TRUE(replaceTextFile(fs, "/s.txt", "two\n", 4));
    TEST_ASSERT_EQUAL_STRING("two\n", fs.files["/s.txt"].c_str());
    TEST_ASSERT_EQUAL_STRING("one\n", fs.files["/s.txt.bak"].c_str());
}

void test_a_full_card_never_destroys_the_last_good_settings() {
    // The defect A09 describes: the old writers removed the live file first,
    // so a failed recreate lost the setting entirely.
    FakeFs fs;
    TEST_ASSERT_TRUE(replaceTextFile(fs, "/s.txt", "good\n", 5));
    fs.writeCap = fs.files["/s.txt"].size(); // no room for anything new
    TEST_ASSERT_FALSE(replaceTextFile(fs, "/s.txt", "better\n", 7));
    TEST_ASSERT_EQUAL_STRING("good\n", fs.files["/s.txt"].c_str());
}

void test_recovery_promotes_a_backup_but_never_a_partial_temp() {
    FakeFs fs;
    fs.files["/s.txt.bak"] = "good\n";
    fs.files["/s.txt.tmp"] = "half-writ";
    TEST_ASSERT_TRUE(recoverTextFile(fs, "/s.txt"));
    TEST_ASSERT_EQUAL_STRING("good\n", fs.files["/s.txt"].c_str());
    TEST_ASSERT_EQUAL_STRING("half-writ", fs.files["/s.txt.tmp"].c_str()); // not promoted

    // With neither a live file nor a backup there is nothing to recover, and
    // that is reported rather than fabricated.
    FakeFs empty;
    TEST_ASSERT_FALSE(recoverTextFile(empty, "/s.txt"));
}

void test_replace_verifies_readback_not_just_the_write_call() {
    FakeFs fs;
    fs.files["/s.txt"] = "good\n";
    fs.writeCap = 8; // accepts the open and part of the payload
    TEST_ASSERT_FALSE(replaceTextFile(fs, "/s.txt", "a much longer value\n", 20));
    TEST_ASSERT_EQUAL_STRING("good\n", fs.files["/s.txt"].c_str());
}

// --- readBoundedLine -----------------------------------------------------

void test_bounded_line_rejects_an_overlong_line_whole() {
    FakeFs fs;
    fs.files["/s.txt"] = "short\nthis one is far too long to fit\nafter\n";
    auto f = fs.open("/s.txt", "r");
    char line[16];
    TEST_ASSERT_TRUE(readBoundedLine(f, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("short", line);
    // A truncated prefix would be a different, valid-looking setting.
    TEST_ASSERT_FALSE(readBoundedLine(f, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("", line);
    // ...and the reader resynchronises on the next line rather than on the
    // tail of the one it rejected.
    TEST_ASSERT_TRUE(readBoundedLine(f, line, sizeof(line)));
    TEST_ASSERT_EQUAL_STRING("after", line);
}

// --- ensureCsvFile (A02) -------------------------------------------------

void test_missing_csv_is_created_with_its_header() {
    FakeFs fs;
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::CREATED);
    TEST_ASSERT_EQUAL_STRING("a,b,c\n", fs.files[PATH].c_str());
}

void test_matching_header_is_adopted_untouched() {
    FakeFs fs;
    fs.files[PATH] = "a,b,c\n1,2,3\n";
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::READY);
    TEST_ASSERT_EQUAL_STRING("a,b,c\n1,2,3\n", fs.files[PATH].c_str());
}

void test_an_unterminated_last_row_is_closed_before_appending() {
    // The power-cut/short-write case. Without this the next append merges
    // into the partial row and produces one plausible-looking hybrid.
    FakeFs fs;
    fs.files[PATH] = "a,b,c\n1,2,3\n4,5";
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::REPAIRED);
    TEST_ASSERT_EQUAL_STRING("a,b,c\n1,2,3\n4,5\n", fs.files[PATH].c_str());
    // The stub row stays visible as a short row; it is not reconstructed.
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::READY);
}

void test_a_header_only_file_is_already_complete() {
    FakeFs fs;
    fs.files[PATH] = "a,b,c\n";
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::READY);
}

void test_an_empty_file_is_rewritten_rather_than_adopted() {
    // SD.exists() was the whole check before A02, so a zero-byte file became
    // a headerless run whose columns nothing downstream could name.
    FakeFs fs;
    fs.files[PATH] = "";
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::CREATED);
    TEST_ASSERT_EQUAL_STRING("a,b,c\n", fs.files[PATH].c_str());
}

void test_a_foreign_schema_is_moved_aside_not_appended_to() {
    FakeFs fs;
    fs.files[PATH] = "x,y,z\n9,9,9\n";
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::REPLACED);
    TEST_ASSERT_EQUAL_STRING("a,b,c\n", fs.files[PATH].c_str());
    // The operator's earlier evidence is preserved, not deleted.
    TEST_ASSERT_EQUAL_STRING("x,y,z\n9,9,9\n", fs.files[std::string(PATH) + ".bad"].c_str());
}

void test_a_truncated_header_is_treated_as_foreign() {
    FakeFs fs;
    fs.files[PATH] = "a,b\n";
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::REPLACED);
    TEST_ASSERT_EQUAL_STRING("a,b,c\n", fs.files[PATH].c_str());
}

void test_a_crlf_header_is_ours() {
    // Some cards come back from a desktop editor with CRLF endings; that is
    // not a different schema.
    FakeFs fs;
    fs.files[PATH] = "a,b,c\r\n1,2,3\n";
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::READY);
}

void test_a_card_that_cannot_write_reports_failure() {
    FakeFs fs;
    fs.openFails = true;
    TEST_ASSERT_TRUE(ensureCsvFile(fs, PATH, HEADER) == CsvFileState::FAILED);

    FakeFs full;
    full.writeCap = 3; // header does not fit
    TEST_ASSERT_TRUE(ensureCsvFile(full, PATH, HEADER) == CsvFileState::FAILED);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_replace_leaves_exactly_one_committed_file);
    RUN_TEST(test_a_full_card_never_destroys_the_last_good_settings);
    RUN_TEST(test_recovery_promotes_a_backup_but_never_a_partial_temp);
    RUN_TEST(test_replace_verifies_readback_not_just_the_write_call);
    RUN_TEST(test_bounded_line_rejects_an_overlong_line_whole);
    RUN_TEST(test_missing_csv_is_created_with_its_header);
    RUN_TEST(test_matching_header_is_adopted_untouched);
    RUN_TEST(test_an_unterminated_last_row_is_closed_before_appending);
    RUN_TEST(test_a_header_only_file_is_already_complete);
    RUN_TEST(test_an_empty_file_is_rewritten_rather_than_adopted);
    RUN_TEST(test_a_foreign_schema_is_moved_aside_not_appended_to);
    RUN_TEST(test_a_truncated_header_is_treated_as_foreign);
    RUN_TEST(test_a_crlf_header_is_ours);
    RUN_TEST(test_a_card_that_cannot_write_reports_failure);
    return UNITY_END();
}
