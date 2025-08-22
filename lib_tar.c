#include "lib_tar.h"
#include <string.h>
#include <stdio.h>

/**
 * Helper used to determine whether a header block is entirely made of
 * NUL bytes which marks the end of a tar archive.
 */
static int is_empty_block(const tar_header_t *hdr) {
    const unsigned char *bytes = (const unsigned char *) hdr;
    for (size_t i = 0; i < sizeof(tar_header_t); i++) {
        if (bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/**
 * Builds the full path of an entry from a tar header.  The resulting string
 * is written into `out` which must be large enough to hold any tar path
 * (256 bytes is sufficient for the ustar format).
 */
static void header_path(char *out, const tar_header_t *hdr) {
    if (hdr->prefix[0] != '\0') {
        snprintf(out, 256, "%s/%s", hdr->prefix, hdr->name);
    } else {
        snprintf(out, 256, "%s", hdr->name);
    }
}

/**
 * Searches for an entry inside the archive.  If found and `header` or
 * `data_offset` are non-NULL, they are populated with the entry header and the
 * offset of the entry data within the file respectively.
 */
static int find_header(int tar_fd, const char *path, tar_header_t *header,
                       off_t *data_offset) {
    tar_header_t hdr;

    if (lseek(tar_fd, 0, SEEK_SET) == (off_t) -1) {
        return 0;
    }

    while (read(tar_fd, &hdr, sizeof(hdr)) == sizeof(hdr)) {
        if (is_empty_block(&hdr)) {
            break;
        }

        char name[256];
        header_path(name, &hdr);

        int match = 0;
        if (hdr.typeflag == DIRTYPE) {
            /* allow searching with or without a trailing slash */
            size_t len = strlen(name);
            if (strcmp(name, path) == 0) {
                match = 1;
            } else if (len > 0 && name[len - 1] == '/' &&
                       strncmp(name, path, len - 1) == 0 && path[len - 1] == '\0') {
                match = 1;
            }
        } else {
            if (strcmp(name, path) == 0) {
                match = 1;
            }
        }

        off_t data_off = lseek(tar_fd, 0, SEEK_CUR);
        if (match) {
            if (header) {
                *header = hdr;
            }
            if (data_offset) {
                *data_offset = data_off;
            }
            return 1;
        }

        size_t size = TAR_INT(hdr.size);
        off_t jump = ((size + 511) / 512) * 512;
        if (lseek(tar_fd, data_off + jump, SEEK_SET) == (off_t) -1) {
            break;
        }
    }

    return 0;
}

/**
 * Resolves a path by following symlinks until a non-symlink entry is found.
 * The resolved header and data offset are returned through `header` and
 * `data_offset` if non-NULL.  The canonical path of the resolved entry is
 * written into `resolved` when provided.
 */
static int resolve_path(int tar_fd, const char *path, tar_header_t *header,
                        off_t *data_offset, char *resolved) {
    char current[256];
    strncpy(current, path, sizeof(current) - 1);
    current[255] = '\0';

    size_t len = strlen(current);
    if (len > 0 && current[len - 1] == '/') {
        current[len - 1] = '\0';
    }

    for (int depth = 0; depth < 16; depth++) {
        off_t off;
        if (!find_header(tar_fd, current, header, &off)) {
            return 0;
        }
        if (header->typeflag == SYMTYPE) {
            strncpy(current, header->linkname, sizeof(current) - 1);
            current[255] = '\0';
            len = strlen(current);
            if (len > 0 && current[len - 1] == '/') {
                current[len - 1] = '\0';
            }
            continue;
        }
        if (resolved) {
            header_path(resolved, header);
        }
        if (data_offset) {
            *data_offset = off;
        }
        return 1;
    }

    return 0;
}

/**
 * Checks whether the archive is valid.
 *
 * Each non-null header of a valid archive has:
 *  - a magic value of "ustar" and a null,
 *  - a version value of "00" and no null,
 *  - a correct checksum
 *
 * @param tar_fd A file descriptor pointing to the start of a file supposed to contain a tar archive.
 *
 * @return a zero or positive value if the archive is valid, representing the number of non-null headers in the archive,
 *         -1 if the archive contains a header with an invalid magic value,
 *         -2 if the archive contains a header with an invalid version value,
 *         -3 if the archive contains a header with an invalid checksum value
 */
int check_archive(int tar_fd) {
    tar_header_t hdr;
    int count = 0;

    if (lseek(tar_fd, 0, SEEK_SET) == (off_t) -1) {
        return -3;
    }

    while (read(tar_fd, &hdr, sizeof(hdr)) == sizeof(hdr)) {
        if (is_empty_block(&hdr)) {
            break;
        }

        if (strncmp(hdr.magic, TMAGIC, TMAGLEN) != 0 || hdr.magic[TMAGLEN - 1] != '\0') {
            return -1;
        }

        if (strncmp(hdr.version, TVERSION, TVERSLEN) != 0) {
            return -2;
        }

        unsigned int expected = TAR_INT(hdr.chksum);

        unsigned char tmp[sizeof(hdr)];
        memcpy(tmp, &hdr, sizeof(hdr));
        memset(tmp + offsetof(tar_header_t, chksum), ' ', 8);

        unsigned int sum = 0;
        for (size_t i = 0; i < sizeof(hdr); i++) {
            sum += tmp[i];
        }

        if (sum != expected) {
            return -3;
        }

        size_t size = TAR_INT(hdr.size);
        off_t jump = ((size + 511) / 512) * 512;
        if (lseek(tar_fd, jump, SEEK_CUR) == (off_t) -1) {
            return -3;
        }

        count++;
    }

    return count;
}

/**
 * Checks whether an entry exists in the archive.
 *
 * @param tar_fd A file descriptor pointing to the start of a valid tar archive file.
 * @param path A path to an entry in the archive.
 *
 * @return zero if no entry at the given path exists in the archive,
 *         any other value otherwise.
 */
int exists(int tar_fd, char *path) {
    return find_header(tar_fd, path, NULL, NULL);
}

/**
 * Checks whether an entry exists in the archive and is a directory.
 *
 * @param tar_fd A file descriptor pointing to the start of a valid tar archive file.
 * @param path A path to an entry in the archive.
 *
 * @return zero if no entry at the given path exists in the archive or the entry is not a directory,
 *         any other value otherwise.
 */
int is_dir(int tar_fd, char *path) {
    tar_header_t hdr;
    if (!find_header(tar_fd, path, &hdr, NULL)) {
        return 0;
    }
    return hdr.typeflag == DIRTYPE;
}

/**
 * Checks whether an entry exists in the archive and is a file.
 *
 * @param tar_fd A file descriptor pointing to the start of a valid tar archive file.
 * @param path A path to an entry in the archive.
 *
 * @return zero if no entry at the given path exists in the archive or the entry is not a file,
 *         any other value otherwise.
 */
int is_file(int tar_fd, char *path) {
    tar_header_t hdr;
    if (!find_header(tar_fd, path, &hdr, NULL)) {
        return 0;
    }
    return hdr.typeflag == REGTYPE || hdr.typeflag == AREGTYPE;
}

/**
 * Checks whether an entry exists in the archive and is a symlink.
 *
 * @param tar_fd A file descriptor pointing to the start of a valid tar archive file.
 * @param path A path to an entry in the archive.
 * @return zero if no entry at the given path exists in the archive or the entry is not symlink,
 *         any other value otherwise.
 */
int is_symlink(int tar_fd, char *path) {
    tar_header_t hdr;
    if (!find_header(tar_fd, path, &hdr, NULL)) {
        return 0;
    }
    return hdr.typeflag == SYMTYPE;
}


/**
 * Lists the entries at a given path in the archive.
 * list() does not recurse into the directories listed at the given path.
 *
 * Example:
 *  dir/          list(..., "dir/", ...) lists "dir/a", "dir/b", "dir/c/" and "dir/e/"
 *   ├── a
 *   ├── b
 *   ├── c/
 *   │   └── d
 *   └── e/
 *
 * @param tar_fd A file descriptor pointing to the start of a valid tar archive file.
 * @param path A path to an entry in the archive. If the entry is a symlink, it must be resolved to its linked-to entry.
 * @param entries An array of char arrays, each one is long enough to contain a tar entry path.
 * @param no_entries An in-out argument.
 *                   The caller set it to the number of entries in `entries`.
 *                   The callee set it to the number of entries listed.
 *
 * @return zero if no directory at the given path exists in the archive,
 *         any other value otherwise.
 */
int list(int tar_fd, char *path, char **entries, size_t *no_entries) {
    size_t capacity = *no_entries;
    *no_entries = 0;

    char base[256];
    tar_header_t hdr;
    if (path && path[0] != '\0') {
        if (!resolve_path(tar_fd, path, &hdr, NULL, base)) {
            return 0;
        }
        if (hdr.typeflag != DIRTYPE) {
            return 0;
        }
        size_t len = strlen(base);
        if (len > 0 && base[len - 1] != '/') {
            base[len] = '/';
            base[len + 1] = '\0';
        }
    } else {
        base[0] = '\0';
    }

    size_t base_len = strlen(base);

    if (lseek(tar_fd, 0, SEEK_SET) == (off_t) -1) {
        return 0;
    }

    size_t count = 0;
    while (read(tar_fd, &hdr, sizeof(hdr)) == sizeof(hdr)) {
        if (is_empty_block(&hdr)) {
            break;
        }

        char name[256];
        header_path(name, &hdr);

        size_t size = TAR_INT(hdr.size);
        off_t data_off = lseek(tar_fd, 0, SEEK_CUR);

        if (strncmp(name, base, base_len) == 0 && strcmp(name, base) != 0) {
            const char *rest = name + base_len;
            const char *slash = strchr(rest, '/');
            if (!slash || slash[1] == '\0') {
                if (count < capacity) {
                    strcpy(entries[count], name);
                    count++;
                }
            }
        }

        off_t jump = ((size + 511) / 512) * 512;
        if (lseek(tar_fd, data_off + jump, SEEK_SET) == (off_t) -1) {
            break;
        }
    }

    *no_entries = count;
    return 1;
}

/**
 * Reads a file at a given path in the archive.
 *
 * @param tar_fd A file descriptor pointing to the start of a valid tar archive file.
 * @param path A path to an entry in the archive to read from.  If the entry is a symlink, it must be resolved to its linked-to entry.
 * @param offset An offset in the file from which to start reading from, zero indicates the start of the file.
 * @param dest A destination buffer to read the given file into.
 * @param len An in-out argument.
 *            The caller set it to the size of dest.
 *            The callee set it to the number of bytes written to dest.
 *
 * @return -1 if no entry at the given path exists in the archive or the entry is not a file,
 *         -2 if the offset is outside the file total length,
 *         zero if the file was read in its entirety into the destination buffer,
 *         a positive value if the file was partially read, representing the remaining bytes left to be read to reach
 *         the end of the file.
 *
 */
ssize_t read_file(int tar_fd, char *path, size_t offset, uint8_t *dest, size_t *len) {
    tar_header_t hdr;
    off_t data_off;
    if (!resolve_path(tar_fd, path, &hdr, &data_off, NULL)) {
        return -1;
    }

    if (!(hdr.typeflag == REGTYPE || hdr.typeflag == AREGTYPE)) {
        return -1;
    }

    size_t size = TAR_INT(hdr.size);
    if (offset > size) {
        return -2;
    }

    size_t to_read = size - offset;
    if (to_read > *len) {
        to_read = *len;
    }

    if (lseek(tar_fd, data_off + offset, SEEK_SET) == (off_t) -1) {
        return -1;
    }

    ssize_t r = read(tar_fd, dest, to_read);
    if (r < 0) {
        return -1;
    }
    *len = (size_t) r;

    if (offset + (size_t) r < size) {
        return size - offset - (size_t) r;
    }
    return 0;
}