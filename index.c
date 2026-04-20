// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    // TODO: Implement index loading
    // (See Lab Appendix for logical steps)
    (void)index;
    return -1;
}

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Helper for qsort to alphabetize index entries by path
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

int index_load(Index *index) {
    index->count = 0;
    
    // Open the index file for reading
    FILE *f = fopen(".pes/index", "r");
    if (!f) {
        // If it doesn't exist, that's fine! It just means a fresh repo.
        return 0; 
    }

    char hash_hex[HASH_HEX_SIZE + 1];
    
    // Read line by line: <mode> <hash> <mtime> <size> <path>
    // %511[^\n] reads the rest of the line (the filepath) up to the newline character
    while (fscanf(f, "%o %64s %lu %u %511[^\n]", 
                  &index->entries[index->count].mode, 
                  hash_hex, 
                  &index->entries[index->count].mtime_sec, 
                  &index->entries[index->count].size, 
                  index->entries[index->count].path) == 5) {
        
        // Convert the hex string back into binary hash format
        hex_to_hash(hash_hex, &index->entries[index->count].hash);
        
        index->count++;
        if (index->count >= MAX_INDEX_ENTRIES) break;
    }
    
    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    // 1. Create a mutable copy and sort it by filepath
    Index sorted_index = *index;
    qsort(sorted_index.entries, sorted_index.count, sizeof(IndexEntry), compare_index_entries);

    // 2. Setup atomic write (temp file)
    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), ".pes/index_tmp_%d", getpid());

    FILE *f = fopen(temp_path, "w");
    if (!f) return -1;

    // 3. Write each entry as text
    for (int i = 0; i < sorted_index.count; i++) {
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted_index.entries[i].hash, hash_hex);
        
        fprintf(f, "%06o %s %lu %u %s\n", 
                sorted_index.entries[i].mode, 
                hash_hex, 
                sorted_index.entries[i].mtime_sec, 
                sorted_index.entries[i].size, 
                sorted_index.entries[i].path);
    }

    // 4. Force data to disk to prevent corruption on crash
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    // 5. Atomically replace the old index with the new one
    if (rename(temp_path, ".pes/index") < 0) {
        return -1;
    }
    
    return 0;
}

int index_add(Index *index, const char *path) {
    // 1. Get file metadata (size, modification time, permissions)
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1; // We only track regular files, not directories

    // 2. Read the file contents into memory
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t *buffer = malloc(st.st_size);
    if (!buffer && st.st_size > 0) { fclose(f); return -1; }
    
    if (st.st_size > 0 && fread(buffer, 1, st.st_size, f) != (size_t)st.st_size) {
        free(buffer); fclose(f); return -1;
    }
    fclose(f);

    // 3. Write the file to our content-addressable store as a BLOB
    ObjectID hash;
    if (object_write(OBJ_BLOB, buffer, st.st_size, &hash) < 0) {
        free(buffer); return -1;
    }
    free(buffer);

    // 4. Update the staging area in memory
    IndexEntry *entry = index_find(index, path);
    if (!entry) {
        // New file: add to the end of the array
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
    }

    // Update the struct with fresh metadata and the new hash
    // 0100755 if executable, 0100644 otherwise
    entry->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->hash = hash;
    entry->mtime_sec = st.st_mtime;
    entry->size = st.st_size;
    snprintf(entry->path, sizeof(entry->path), "%s", path);

    // 5. Save the updated index back to disk
    return index_save(index);
}
