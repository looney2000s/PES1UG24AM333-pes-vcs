// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
// Recursive helper function signature
static int write_tree_level(const IndexEntry *entries, int count, int path_offset, ObjectID *out_id) {
    Tree current_tree;
    current_tree.count = 0;

    int i = 0;
    while (i < count && current_tree.count < MAX_TREE_ENTRIES) {
        // Look at the path from our current directory depth
        const char *local_path = entries[i].path + path_offset;
        const char *slash = strchr(local_path, '/');

        if (!slash) {
            // BASE CASE: No slash means it's a file in the current directory.
            TreeEntry *te = &current_tree.entries[current_tree.count++];
            te->mode = entries[i].mode;
            
            // strncpy is safer, but we know the limits from the struct
            strncpy(te->name, local_path, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            
            te->hash = entries[i].hash;
            i++; 
        } else {
            // RECURSIVE CASE: It's a subdirectory.
            size_t dir_len = slash - local_path;
            char dir_name[256];
            strncpy(dir_name, local_path, dir_len);
            dir_name[dir_len] = '\0';

            // Scan ahead to find all files belonging to this subdirectory
            int j = i + 1;
            while (j < count) {
                const char *next_local = entries[j].path + path_offset;
                // Check if the next path starts with "dir_name/"
                if (strncmp(next_local, dir_name, dir_len) == 0 && next_local[dir_len] == '/') {
                    j++; // It belongs to this directory, keep scanning
                } else {
                    break; // We've hit a file in a different directory
                }
            }

            // Recursively build the subtree for this chunk of files
            ObjectID sub_tree_id;
            int sub_count = j - i;
            // Advance path_offset past "dir_name/" (+1 for the slash)
            write_tree_level(&entries[i], sub_count, path_offset + dir_len + 1, &sub_tree_id);

            // Add the newly created directory tree to our current tree
            TreeEntry *te = &current_tree.entries[current_tree.count++];
            te->mode = MODE_DIR; // 0040000
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            te->hash = sub_tree_id;

            // Jump 'i' forward past all the files we just processed
            i = j;
        }
    }

    // Serialize the fully constructed tree into a binary format
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&current_tree, &tree_data, &tree_len) < 0) {
        return -1;
    }

    // Write the binary tree object to the .pes/objects/ store
    if (object_write(OBJ_TREE, tree_data, tree_len, out_id) < 0) {
        free(tree_data);
        return -1;
    }

    free(tree_data);
    return 0;
}

int tree_from_index(ObjectID *id_out) {
    Index idx;
    // Load the current staging area
    if (index_load(&idx) < 0) {
        return -1;
    }

    if (idx.count == 0) {
        // Edge case: Empty repository
        // (We would normally create an empty tree here, but let's 
        // rely on the recursive function to handle zero entries)
    }

    // Kick off the recursion starting at character offset 0
    return write_tree_level(idx.entries, idx.count, 0, id_out);
}

static int write_tree_level(const IndexEntry *entries, int count, int path_offset, ObjectID *out_id) {
    Tree current_tree;
    current_tree.count = 0;

    int i = 0;
    while (i < count && current_tree.count < MAX_TREE_ENTRIES) {
        // TODO: Logic to detect if entries[i] is a file or a directory
        // TODO: Recursive grouping logic
        
        i++; // Temporary infinite loop prevention
    }

    // TODO: Serialize and write current_tree to object store
    return -1;
}
