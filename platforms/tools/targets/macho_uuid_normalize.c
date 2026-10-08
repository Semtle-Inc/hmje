#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *message) {
    fprintf(stderr, "macho-uuid-normalize: %s: %s\n", message, strerror(errno));
    return 1;
}

static uint32_t read_be32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
           (uint32_t)bytes[2] << 8 | bytes[3];
}

// A signed inode can remain invalid in the kernel after an in-place write,
// even when only unsigned signature padding changed. Publish a new inode.
static int publish_normalized(const char *path, const uint8_t *bytes, size_t size, mode_t mode) {
    size_t capacity = strlen(path) + sizeof(".hmje-normalize-XXXXXX");
    char *temporary = malloc(capacity);
    if (!temporary) return -1;
    snprintf(temporary, capacity, "%s.hmje-normalize-XXXXXX", path);
    int output = mkstemp(temporary);
    if (output < 0) { free(temporary); return -1; }
    int status = fchmod(output, mode & 0777);
    size_t position = 0;
    while (status == 0 && position < size) {
        ssize_t count = write(output, bytes + position, size - position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count == 0) errno = EIO; status = -1; break; }
        position += (size_t)count;
    }
    if (status == 0) status = fsync(output);
    int saved = errno;
    if (close(output) != 0 && status == 0) { status = -1; saved = errno; }
    if (status == 0 && rename(temporary, path) != 0) { status = -1; saved = errno; }
    if (status != 0) unlink(temporary);
    free(temporary);
    errno = saved;
    return status;
}

int main(int argc, char **argv) {
    int signature_padding = argc == 3 && strcmp(argv[1], "--signature-padding") == 0;
    if (argc != 2 && !signature_padding) {
        fprintf(stderr, "usage: macho-uuid-normalize [--signature-padding] <thin-mach-o>\n");
        return 2;
    }

    const char *path = argv[signature_padding ? 2 : 1];
    int fd = open(path, signature_padding ? O_RDONLY : O_RDWR);
    if (fd < 0) return fail("open");

    struct stat info;
    if (fstat(fd, &info) != 0) {
        close(fd);
        return fail("fstat");
    }
    if (!S_ISREG(info.st_mode) || info.st_size < (off_t)sizeof(struct mach_header_64)) {
        close(fd);
        errno = EINVAL;
        return fail("file is not a 64-bit Mach-O");
    }

    uint8_t *bytes = mmap(NULL, (size_t)info.st_size, PROT_READ | PROT_WRITE,
                          signature_padding ? MAP_PRIVATE : MAP_SHARED, fd, 0);
    if (bytes == MAP_FAILED) {
        close(fd);
        return fail("mmap");
    }

    struct mach_header_64 *header = (struct mach_header_64 *)bytes;
    if (header->magic != MH_MAGIC_64 ||
        sizeof(*header) + header->sizeofcmds > (uint64_t)info.st_size) {
        munmap(bytes, (size_t)info.st_size);
        close(fd);
        errno = EINVAL;
        return fail("unsupported Mach-O layout");
    }

    uint8_t *cursor = bytes + sizeof(*header);
    uint8_t *commands_end = cursor + header->sizeofcmds;
    struct uuid_command *uuid = NULL;
    struct linkedit_data_command *signature = NULL;
    const char *problem = NULL;
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if ((size_t)(commands_end - cursor) < sizeof(struct load_command)) {
            problem = "truncated load command";
            break;
        }
        struct load_command *command = (struct load_command *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(commands_end - cursor)) {
            problem = "invalid load command size";
            break;
        }
        if (command->cmd == LC_UUID) {
            if (uuid || command->cmdsize < sizeof(*uuid)) {
                problem = "invalid or repeated LC_UUID";
                break;
            }
            uuid = (struct uuid_command *)cursor;
        }
        if (command->cmd == LC_CODE_SIGNATURE) {
            if (signature || command->cmdsize < sizeof(*signature)) {
                problem = "invalid or repeated LC_CODE_SIGNATURE";
                break;
            }
            signature = (struct linkedit_data_command *)cursor;
        }
        cursor += command->cmdsize;
    }
    if (!problem && cursor != commands_end) problem = "inconsistent load command count";

    uint8_t *padding = NULL;
    size_t padding_size = 0;
    if (!problem && signature_padding) {
        if (!signature || signature->dataoff < sizeof(*header) + header->sizeofcmds ||
            signature->datasize < 12 ||
            (uint64_t)signature->dataoff + signature->datasize != (uint64_t)info.st_size) {
            problem = "invalid final code signature allocation";
        } else {
            uint8_t *blob = bytes + signature->dataoff;
            uint32_t length = read_be32(blob + 4);
            uint32_t count = read_be32(blob + 8);
            if (read_be32(blob) != 0xfade0cc0 || length < 12 ||
                length > signature->datasize || count > (length - 12) / 8) {
                problem = "invalid signature SuperBlob";
            } else {
                for (uint32_t index = 0; index < count; ++index) {
                    uint32_t offset = read_be32(blob + 12 + index * 8 + 4);
                    if (offset < 12 + count * 8 || offset > length - 8) {
                        problem = "invalid signature sub-blob offset";
                        break;
                    }
                    uint32_t size = read_be32(blob + offset + 4);
                    if (size < 8 || size > length - offset) {
                        problem = "invalid signature sub-blob length";
                        break;
                    }
                }
                padding = blob + length;
                padding_size = signature->datasize - length;
            }
        }
    } else if (!problem && !uuid) {
        problem = "LC_UUID was not found";
    }
    if (problem) {
        munmap(bytes, (size_t)info.st_size);
        close(fd);
        errno = EINVAL;
        return fail(problem);
    }
    // Signature padding lies outside every signed byte and every active blob.
    // Do not alter a load command or the UUID after codesign has signed the file.
    if (signature_padding) memset(padding, 0, padding_size);
    else memset(uuid->uuid, 0, sizeof(uuid->uuid));
    int saved_result = signature_padding
        ? publish_normalized(path, bytes, (size_t)info.st_size, info.st_mode)
        : msync(bytes, (size_t)info.st_size, MS_SYNC);
    if (saved_result != 0) {
        munmap(bytes, (size_t)info.st_size);
        close(fd);
        return fail(signature_padding ? "atomic signature padding publication" : "msync");
    }
    munmap(bytes, (size_t)info.st_size);
    if (close(fd) != 0) return fail("close");
    return 0;
}
