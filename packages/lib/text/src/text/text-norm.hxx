#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace text_norm
{

std::vector<std::string> words(const std::string& text, int minAlnum = 3);

std::unordered_set<std::string> wordSet(const std::string& text,
                                        int minAlnum = 4);

std::string whitespace(const std::string& text, bool toLower = true);

std::string stripAccents(std::string text);

std::string intent(const std::string& text);

}
