/* Streaming gzip and OpenSSL SHA-256; strict single-member gzip verification. */
#include "vinylscan.h"
#include <errno.h>
#include <openssl/evp.h>
#include <string.h>
#include <zlib.h>

static int digest_finish(EVP_MD_CTX *context, char hash[65])
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 32) return -1;
    for (unsigned int i = 0; i < length; i++) snprintf(hash + i * 2, 3, "%02x", digest[i]);
    return 0;
}

/* Feed preview, EVP and deflate from the same read buffer. Bound reads by the
 * inventoried size so a growing live file cannot keep this loop running forever. */
int stream_file(int source, int output, Entry *entry, uint64_t *bytes)
{
    unsigned char input[IO_SIZE], compressed[IO_SIZE], sample[PREVIEW_SAMPLE];
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    z_stream stream = {0};
    bool initialized = false;
    int result = -1;
    *bytes = 0;
    entry->preview.sample = sample;
    if (!digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1) { errno = ENOMEM; goto done; }
    if (output >= 0) {
        if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            errno = ENOMEM; goto done;
        }
        initialized = true;
    }
    for (;;) {
        ssize_t count = read_retry(source, input, sizeof(input));
        if (count < 0) goto done;
        if (UINT64_MAX - *bytes < (uint64_t)count) { errno = EOVERFLOW; goto done; }
        *bytes += (uint64_t)count;
        if (*bytes > (uint64_t)entry->stat.st_size) { errno = ESTALE; goto done; }
        preview_add(&entry->preview, input, (size_t)count);
        if (EVP_DigestUpdate(digest, input, (size_t)count) != 1) { errno = EIO; goto done; }
        if (output >= 0) {
            stream.next_in = input;
            stream.avail_in = (uInt)count;
            int status;
            do {
                stream.next_out = compressed;
                stream.avail_out = sizeof(compressed);
                status = deflate(&stream, count ? Z_NO_FLUSH : Z_FINISH);
                if (status != Z_OK && status != Z_STREAM_END) { errno = EIO; goto done; }
                if (write_all(output, compressed, sizeof(compressed) - stream.avail_out) < 0) goto done;
            } while (status != Z_STREAM_END &&
                     (stream.avail_in || !count || !stream.avail_out));
        }
        if (!count) break;
    }
    if (digest_finish(digest, entry->sha256) < 0) { errno = EIO; goto done; }
    preview_finish(&entry->preview);
    result = 0;
done:
    entry->preview.sample = NULL;
    if (initialized) deflateEnd(&stream);
    EVP_MD_CTX_free(digest);
    return result;
}

/* Use inflate rather than gzread's transparent/plaintext mode. The output limit
 * also stops expansion beyond the expected file size when checking a bad gzip. */
int hash_gzip(int fd, char hash[65], uint64_t *size, uint64_t maximum)
{
    unsigned char input[IO_SIZE], output[IO_SIZE];
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    z_stream stream = {0};
    bool initialized = false;
    bool eof = false;
    int result = -1;
    *size = 0;
    if (!digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1) { errno = ENOMEM; goto done; }
    if (inflateInit2(&stream, 15 + 16) != Z_OK) { errno = ENOMEM; goto done; }
    initialized = true;
    for (;;) {
        if (!stream.avail_in && !eof) {
            ssize_t count = read_retry(fd, input, sizeof(input));
            if (count < 0) goto done;
            if (!count) eof = true;
            stream.next_in = input;
            stream.avail_in = (uInt)count;
        }
        stream.next_out = output;
        stream.avail_out = sizeof(output);
        int status = inflate(&stream, Z_NO_FLUSH);
        size_t produced = sizeof(output) - stream.avail_out;
        if (UINT64_MAX - *size < produced) { errno = EOVERFLOW; goto done; }
        *size += produced;
        if (*size > maximum) { errno = EBADMSG; goto done; }
        if (EVP_DigestUpdate(digest, output, produced) != 1) { errno = EIO; goto done; }
        if (status == Z_STREAM_END) {
            /* Reject transparent plaintext, extra members and trailing garbage. */
            if (stream.avail_in) { errno = EBADMSG; goto done; }
            ssize_t extra = read_retry(fd, input, 1);
            if (extra < 0) goto done;
            if (extra) { errno = EBADMSG; goto done; }
            break;
        }
        if (status != Z_OK) { errno = EBADMSG; goto done; }
    }
    if (digest_finish(digest, hash) < 0) { errno = EIO; goto done; }
    result = 0;
done:
    if (initialized) inflateEnd(&stream);
    EVP_MD_CTX_free(digest);
    return result;
}
