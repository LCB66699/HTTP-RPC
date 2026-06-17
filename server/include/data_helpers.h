#pragma once
#include <string>

// Count rows in a JSON 2D array: [[...],[...]]
inline int countRows(const std::string &json) {
    if (json.empty() || json == "[]")
        return 0;
    int n = 1;
    for (size_t i = 0; i < json.size(); ++i)
        if (json[i] == ']' && i + 2 < json.size() && json[i + 1] == ',' && json[i + 2] == '[')
            n++;
    return n;
}

// Count columns in a JSON array: ["A","B","C"]
// Handles quoted commas that should not count as delimiters.
inline int countCols(const std::string &json) {
    if (json.empty() || json == "[]")
        return 0;
    int n = 1;
    bool in_string = false;
    for (size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '"' && (i == 0 || json[i - 1] != '\\'))
            in_string = !in_string;
        if (!in_string && json[i] == ',')
            n++;
    }
    return n;
}
