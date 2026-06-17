#include <gtest/gtest.h>
#include "../server/include/data_helpers.h"

TEST(DataHelpers, CountRowsEmpty) {
    EXPECT_EQ(countRows(""), 0);
    EXPECT_EQ(countRows("[]"), 0);
}

TEST(DataHelpers, CountRowsSingle) {
    EXPECT_EQ(countRows("[[]]"), 1);
    EXPECT_EQ(countRows("[[\"a\"]]"), 1);
}

TEST(DataHelpers, CountRowsMultiple) {
    EXPECT_EQ(countRows("[[1],[2],[3]]"), 3);
    EXPECT_EQ(countRows("[[\"a\",\"b\"],[\"c\",\"d\"]]"), 2);
}

TEST(DataHelpers, CountColsEmpty) {
    EXPECT_EQ(countCols(""), 0);
    EXPECT_EQ(countCols("[]"), 0);
}

TEST(DataHelpers, CountColsSimple) {
    EXPECT_EQ(countCols("[\"A\"]"), 1);
    EXPECT_EQ(countCols("[\"A\",\"B\",\"C\"]"), 3);
    EXPECT_EQ(countCols("[1,2,3,4,5]"), 5);
}

TEST(DataHelpers, CountColsQuotedComma) {
    // Comma inside quoted string should not count as separator
    EXPECT_EQ(countCols("[\"hello, world\",\"B\"]"), 2);
}

TEST(DataHelpers, CountColsEscapedQuote) {
    // Escaped quote \\" should not toggle in_string
    EXPECT_EQ(countCols("[\"a\\\"b\",\"c\"]"), 2);
}
