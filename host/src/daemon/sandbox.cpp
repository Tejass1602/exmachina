#include "daemon/sandbox.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>

/* ─── Simple Unified Diff Parser ─── */
struct DiffHunk {
    std::string file;
    int old_start, old_count;
    int new_start, new_count;
    std::vector<std::string> lines;  // prefixed with ' ', '-', '+'
};

static bool parse_unified_diff(const char* diff_text, size_t diff_len, std::vector<DiffHunk>& hunks) {
    std::string diff(diff_text, diff_len);
    std::istringstream iss(diff);
    std::string line;
    
    DiffHunk* current = nullptr;
    std::string current_file;
    
    while (std::getline(iss, line)) {
        if (line.size() >= 4 && line.substr(0, 4) == "--- ") {
            // Old file
            current_file = line.substr(4);
            if (current_file.size() >= 2 && current_file.substr(0, 2) == "a/") {
                current_file = current_file.substr(2);
            }
        } else if (line.size() >= 4 && line.substr(0, 4) == "+++ ") {
            // New file
            std::string new_file = line.substr(4);
            if (new_file.size() >= 2 && new_file.substr(0, 2) == "b/") {
                new_file = new_file.substr(2);
            }
            if (current_file.empty()) current_file = new_file;
        } else if (line.size() >= 3 && line.substr(0, 3) == "@@ ") {
            // Hunk header: @@ -old_start,old_count +new_start,new_count @@
            if (current) hunks.push_back(*current);
            current = new DiffHunk();
            current->file = current_file;
            
            // Parse @@ -1,2 +1,3 @@
            size_t pos = 3;
            while (pos < line.size() && line[pos] == ' ') pos++;
            if (line[pos] != '-') return false;
            pos++;
            
            size_t comma = line.find(',', pos);
            if (comma == std::string::npos) {
                current->old_start = std::stoi(line.substr(pos));
                current->old_count = 1;
            } else {
                current->old_start = std::stoi(line.substr(pos, comma - pos));
                current->old_count = std::stoi(line.substr(comma + 1));
            }
            
            pos = line.find('+', pos);
            if (pos == std::string::npos) return false;
            pos++;
            
            comma = line.find(',', pos);
            if (comma == std::string::npos) {
                current->new_start = std::stoi(line.substr(pos));
                current->new_count = 1;
            } else {
                current->new_start = std::stoi(line.substr(pos, comma - pos));
                current->new_count = std::stoi(line.substr(comma + 1));
            }
        } else if (current && !line.empty()) {
            current->lines.push_back(line);
        }
    }
    if (current) hunks.push_back(*current);
    
    return true;
}

static bool apply_hunk(std::vector<std::string>& file_lines, const DiffHunk& hunk) {
    // Convert to 0-based
    int old_start = hunk.old_start - 1;
    int old_end = old_start + hunk.old_count;
    
    if (old_start < 0 || old_end > (int)file_lines.size()) {
        return false;
    }
    
    // Verify context/removed lines match
    int file_idx = old_start;
    for (size_t i = 0; i < hunk.lines.size(); ++i) {
        const std::string& l = hunk.lines[i];
        if (l.empty()) continue;
        char prefix = l[0];
        std::string content = l.substr(1);
        
        if (prefix == ' ' || prefix == '-') {
            // Context or removed line - must match current file line
            if (file_idx >= (int)file_lines.size() || file_lines[file_idx] != content) {
                return false;
            }
            file_idx++;
        }
        // '+' lines don't consume file lines
    }
    
    if (file_idx != old_end) {
        return false;  // Didn't consume all expected lines
    }
    
    // Build new lines
    std::vector<std::string> new_lines;
    for (int i = 0; i < old_start; ++i) {
        new_lines.push_back(file_lines[i]);
    }
    for (const auto& l : hunk.lines) {
        if (!l.empty() && (l[0] == ' ' || l[0] == '+')) {
            new_lines.push_back(l.substr(1));
        }
    }
    for (size_t i = old_end; i < file_lines.size(); ++i) {
        new_lines.push_back(file_lines[i]);
    }
    
    file_lines.swap(new_lines);
    return true;
}

/* ─── Sandbox Implementation ─── */
SandboxResult sandbox_apply_and_compile(const char* diff_text, size_t diff_len) {
    SandboxResult result = {false, ""};
    
    // Create temporary directory
    char tmpl[] = "/tmp/exm-sandbox-XXXXXX";
    char* sandbox_dir = mkdtemp(tmpl);
    if (!sandbox_dir) {
        snprintf(result.error, sizeof(result.error), "Failed to create sandbox directory");
        return result;
    }
    
    // Parse diff
    std::vector<DiffHunk> hunks;
    if (!parse_unified_diff(diff_text, diff_len, hunks)) {
        snprintf(result.error, sizeof(result.error), "Failed to parse unified diff");
        rmdir(sandbox_dir);
        return result;
    }
    
    // For simplicity, we'll assume single-file diffs for now
    // In production, you'd iterate over files
    if (hunks.empty()) {
        snprintf(result.error, sizeof(result.error), "No hunks in diff");
        rmdir(sandbox_dir);
        return result;
    }
    
    // Get target file from first hunk
    std::string target_file = hunks[0].file;
    if (target_file.empty()) {
        snprintf(result.error, sizeof(result.error), "No target file in diff");
        rmdir(sandbox_dir);
        return result;
    }
    
    // Copy target file to sandbox
    std::string sandbox_file = std::string(sandbox_dir) + "/" + target_file;
    
    // Read original file
    FILE* fp = fopen(target_file.c_str(), "r");
    if (!fp) {
        snprintf(result.error, sizeof(result.error), "Cannot open target file: %s", target_file.c_str());
        rmdir(sandbox_dir);
        return result;
    }
    
    std::vector<std::string> lines;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), fp)) {
        // Remove trailing newline
        buffer[strcspn(buffer, "\n")] = '\0';
        lines.push_back(buffer);
    }
    fclose(fp);
    
    // Apply all hunks
    for (const auto& hunk : hunks) {
        if (!apply_hunk(lines, hunk)) {
            snprintf(result.error, sizeof(result.error), "Failed to apply hunk to %s", target_file.c_str());
            rmdir(sandbox_dir);
            return result;
        }
    }
    
    // Write patched file
    fp = fopen(sandbox_file.c_str(), "w");
    if (!fp) {
        snprintf(result.error, sizeof(result.error), "Cannot write patched file");
        rmdir(sandbox_dir);
        return result;
    }
    for (const auto& l : lines) {
        fprintf(fp, "%s\n", l.c_str());
    }
    fclose(fp);
    
    // Determine compiler based on extension
    std::string compiler = "g++";
    std::string flags = "-std=c++20 -Wall -Wextra -O2";
    if (target_file.size() >= 3 && target_file.substr(target_file.size() - 3) == ".cu") {
        compiler = "nvcc";
        flags = "-std=c++20 -arch=sm_86 -O3 -lineinfo";
    } else if (target_file.size() >= 2 && target_file.substr(target_file.size() - 2) == ".c") {
        compiler = "gcc";
        flags = "-std=c11 -Wall -Wextra -O2";
    }
    
    // Compile
    std::string cmd = compiler + " " + flags + " -c " + sandbox_file + " -o " + sandbox_dir + "/test.o 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        snprintf(result.error, sizeof(result.error), "Failed to run compiler");
        rmdir(sandbox_dir);
        return result;
    }
    
    char output[4096] = {0};
    size_t out_len = 0;
    while (fgets(buffer, sizeof(buffer), pipe) && out_len < sizeof(output) - 1) {
        strncat(output, buffer, sizeof(output) - out_len - 1);
        out_len = strlen(output);
    }
    int status = pclose(pipe);
    
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        result.success = true;
    } else {
        snprintf(result.error, sizeof(result.error), "Compilation failed: %s", output);
    }
    
    // Cleanup
    unlink(sandbox_file.c_str());
    unlink((std::string(sandbox_dir) + "/test.o").c_str());
    rmdir(sandbox_dir);
    
    return result;
}