/*
 * ZIP support routines for PhysicsFS.
 *
 * Please see the file LICENSE in the source's root directory.
 *
 *  This file written by Ryan C. Gordon, with some peeking at "unzip.c"
 *   by Gilles Vollant.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#if (defined PHYSFS_SUPPORTS_ZIP)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <errno.h>

#include "physfs.h"
#include "zlib.h"

#define __PHYSICSFS_INTERNAL__
#include "physfs_internal.h"

/*
 * When sorting the zip entries in an archive, we use a modified QuickSort.
 *  When there are less then ZIP_QUICKSORT_THRESHOLD entries left to sort,
 *  we switch over to an InsertionSort for the remainder. Tweak to taste.
 */
#define ZIP_QUICKSORT_THRESHOLD 4

/*
 * A buffer of ZIP_READBUFSIZE is malloc() for each compressed file opened,
 *  and is free()'d when you close the file; compressed data is read into
 *  this buffer, and then is decompressed into the buffer passed to
 *  PHYSFS_read().
 *
 * Uncompressed entries in a zipfile do not allocate this buffer; they just
 *  read data directly into the buffer passed to PHYSFS_read().
 *
 * Depending on your speed and memory requirements, you should tweak this
 *  value.
 */
#define ZIP_READBUFSIZE   (16 * 1024)


/*
 * Entries are "unresolved" until they are first opened. At that time,
 *  local file headers parsed/validated, data offsets will be updated to look
 *  at the actual file data instead of the header, and symlinks will be
 *  followed and optimized. This means that we don't seek and read around the
 *  archive until forced to do so, and after the first time, we had to do
 *  less reading and parsing, which is very CD-ROM friendly.
 */
typedef enum
{
    ZIP_UNRESOLVED_FILE,
    ZIP_UNRESOLVED_SYMLINK,
    ZIP_RESOLVING,
    ZIP_RESOLVED,
    ZIP_BROKEN_FILE,
    ZIP_BROKEN_SYMLINK,
} ZipResolveType;


/*
 * One ZIPentry is kept for each file in an open ZIP archive.
 */
typedef struct _ZIPentry
{
    char *name;                         /* Name of file in archive        */
    struct _ZIPentry *symlink;          /* NULL or file we symlink to     */
    ZipResolveType resolved;            /* Have we resolved file/symlink? */
    PHYSFS_uint32 offset;               /* offset of data in archive      */
    PHYSFS_uint16 version;              /* version made by                */
    PHYSFS_uint16 version_needed;       /* version needed to extract      */
    PHYSFS_uint16 compression_method;   /* compression method             */
    PHYSFS_uint32 crc;                  /* crc-32                         */
    PHYSFS_uint32 compressed_size;      /* compressed size                */
    PHYSFS_uint32 uncompressed_size;    /* uncompressed size              */
    PHYSFS_sint64 last_mod_time;        /* last file mod time             */
} ZIPentry;

/*
 * One ZIPinfo is kept for each open ZIP archive.
 */
typedef struct
{
    char *archiveName;        /* path to ZIP in platform-dependent notation. */
    PHYSFS_uint16 entryCount; /* Number of files in ZIP.                     */
    ZIPentry *entries;        /* info on all files in ZIP.                   */
} ZIPinfo;

/*
 * One ZIPfileinfo is kept for each open file in a ZIP archive.
 */
typedef struct
{
    ZIPentry *entry;                      /* Info on file.              */
    void *handle;                         /* physical file handle.      */
    PHYSFS_uint32 compressed_position;    /* offset in compressed data. */
    PHYSFS_uint32 uncompressed_position;  /* tell() position.           */
    PHYSFS_uint8 *buffer;                 /* decompression buffer.      */
    z_stream stream;                      /* zlib stream state.         */
} ZIPfileinfo;


/* Magic numbers... */
#define ZIP_LOCAL_FILE_SIG          0x04034b50
#define ZIP_CENTRAL_DIR_SIG         0x02014b50
#define ZIP_END_OF_CENTRAL_DIR_SIG  0x06054b50

/* compression methods... */
#define COMPMETH_NONE 0
/* ...and others... */


#define UNIX_FILETYPE_MASK    0170000
#define UNIX_FILETYPE_SYMLINK 0120000


static PHYSFS_sint64 ZIP_read(FileHandle *handle, void *buffer,
                              PHYSFS_uint32 objSize, PHYSFS_uint32 objCount);
static int ZIP_eof(FileHandle *handle);
static PHYSFS_sint64 ZIP_tell(FileHandle *handle);
static int ZIP_seek(FileHandle *handle, PHYSFS_uint64 offset);
static PHYSFS_sint64 ZIP_fileLength(FileHandle *handle);
static int ZIP_fileClose(FileHandle *handle);
static int ZIP_isArchive(const char *filename, int forWriting);
static DirHandle *ZIP_openArchive(const char *name, int forWriting);
static LinkedStringList *ZIP_enumerateFiles(DirHandle *h,
                                            const char *dirname,
                                            int omitSymLinks);
static int ZIP_exists(DirHandle *h, const char *name);
static int ZIP_isDirectory(DirHandle *h, const char *name);
static int ZIP_isSymLink(DirHandle *h, const char *name);
static PHYSFS_sint64 ZIP_getLastModTime(DirHandle *h, const char *name);
static FileHandle *ZIP_openRead(DirHandle *h, const char *filename);
static void ZIP_dirClose(DirHandle *h);

static int zip_resolve(void *in, ZIPinfo *info, ZIPentry *entry);


static const FileFunctions __PHYSFS_FileFunctions_ZIP =
{
    ZIP_read,       /* read() method       */
    NULL,           /* write() method      */
    ZIP_eof,        /* eof() method        */
    ZIP_tell,       /* tell() method       */
    ZIP_seek,       /* seek() method       */
    ZIP_fileLength, /* fileLength() method */
    ZIP_fileClose   /* fileClose() method  */
};


const DirFunctions __PHYSFS_DirFunctions_ZIP =
{
    ZIP_isArchive,          /* isArchive() method      */
    ZIP_openArchive,        /* openArchive() method    */
    ZIP_enumerateFiles,     /* enumerateFiles() method */
    ZIP_exists,             /* exists() method         */
    ZIP_isDirectory,        /* isDirectory() method    */
    ZIP_isSymLink,          /* isSymLink() method      */
    ZIP_getLastModTime,     /* getLastModTime() method */
    ZIP_openRead,           /* openRead() method       */
    NULL,                   /* openWrite() method      */
    NULL,                   /* openAppend() method     */
    NULL,                   /* remove() method         */
    NULL,                   /* mkdir() method          */
    ZIP_dirClose            /* dirClose() method       */
};

const PHYSFS_ArchiveInfo __PHYSFS_ArchiveInfo_ZIP =
{
    "ZIP",
    "PkZip/WinZip/Info-Zip compatible",
    "Ryan C. Gordon <icculus@clutteredmind.org>",
    "http://www.icculus.org/physfs/",
};



/*
 * Wrap all zlib calls in this, so the physfs error state is set appropriately.
 */
static int zlib_err(int rc)
{
    const char *err = NULL;

    switch (rc)
    {
        case Z_OK:
        case Z_STREAM_END:
            break;   /* not errors. */

        case Z_ERRNO:
            err = strerror(errno);
            break;

        case Z_NEED_DICT:
            err = "zlib: need dictionary";
            break;
        case Z_DATA_ERROR:
            err = "zlib: need dictionary";
            break;
        case Z_MEM_ERROR:
            err = "zlib: memory error";
            break;
        case Z_BUF_ERROR:
            err = "zlib: buffer error";
            break;
        case Z_VERSION_ERROR:
            err = "zlib: version error";
            break;
        default:
            err = "unknown zlib error";
            break;
    } /* switch */

    if (err != NULL)
        __PHYSFS_setError(err);

    return(rc);
} /* zlib_err */


/*
 * Read an unsigned 32-bit int and swap to native byte order.
 */
static int readui32(void *in, PHYSFS_uint32 *val)
{
    PHYSFS_uint32 v;
    BAIL_IF_MACRO(__PHYSFS_platformRead(in, &v, sizeof (v), 1) != 1, NULL, 0);
    *val = PHYSFS_swapULE32(v);
    return(1);
} /* readui32 */


/*
 * Read an unsigned 16-bit int and swap to native byte order.
 */
static int readui16(void *in, PHYSFS_uint16 *val)
{
    PHYSFS_uint16 v;
    BAIL_IF_MACRO(__PHYSFS_platformRead(in, &v, sizeof (v), 1) != 1, NULL, 0);
    *val = PHYSFS_swapULE16(v);
    return(1);
} /* readui16 */


static PHYSFS_sint64 ZIP_read(FileHandle *handle, void *buf,
                              PHYSFS_uint32 objSize, PHYSFS_uint32 objCount)
{
    ZIPfileinfo *finfo = (ZIPfileinfo *) (handle->opaque);
    ZIPentry *entry = finfo->entry;
    PHYSFS_sint64 retval = 0;
    PHYSFS_sint64 maxread = ((PHYSFS_sint64) objSize) * objCount;
    PHYSFS_sint64 avail = entry->uncompressed_size -
                          finfo->uncompressed_position;

    BAIL_IF_MACRO(maxread == 0, NULL, 0);    /* quick rejection. */

    if (avail < maxread)
    {
        maxread = avail - (avail % objSize);
        objCount = maxread / objSize;
        BAIL_IF_MACRO(objCount == 0, ERR_PAST_EOF, 0);  /* quick rejection. */
        __PHYSFS_setError(ERR_PAST_EOF);   /* this is always true here. */
    } /* if */

    if (entry->compression_method == COMPMETH_NONE)
    {
        retval = __PHYSFS_platformRead(finfo->handle, buf, objSize, objCount);
    } /* if */

    else
    {
        finfo->stream.next_out = buf;
        finfo->stream.avail_out = objSize * objCount;

        while (retval < maxread)
        {
            PHYSFS_uint32 before = finfo->stream.total_out;
            int rc;

            if (finfo->stream.avail_in == 0)
            {
                PHYSFS_sint64 br;

                br = entry->compressed_size - finfo->compressed_position;
                if (br > 0)
                {
                    if (br > ZIP_READBUFSIZE)
                        br = ZIP_READBUFSIZE;

                    br = __PHYSFS_platformRead(finfo->handle,
                                               finfo->buffer,
                                               1, br);
                    if (br <= 0)
                        break;

                    finfo->compressed_position += br;
                    finfo->stream.next_in = finfo->buffer;
                    finfo->stream.avail_in = br;
                } /* if */
            } /* if */

            rc = zlib_err(inflate(&finfo->stream, Z_SYNC_FLUSH));
            retval += (finfo->stream.total_out - before);

            if (rc != Z_OK)
                break;
        } /* while */

        retval /= objSize;
    } /* else */

    if (retval > 0)
        finfo->uncompressed_position += (retval * objSize);

    return(retval);
} /* ZIP_read */


static int ZIP_eof(FileHandle *handle)
{
    ZIPfileinfo *finfo = ((ZIPfileinfo *) (handle->opaque));
    return(finfo->uncompressed_position >= finfo->entry->uncompressed_size);
} /* ZIP_eof */


static PHYSFS_sint64 ZIP_tell(FileHandle *handle)
{
    return(((ZIPfileinfo *) (handle->opaque))->uncompressed_position);
} /* ZIP_tell */


static int ZIP_seek(FileHandle *handle, PHYSFS_uint64 offset)
{
    ZIPfileinfo *finfo = (ZIPfileinfo *) (handle->opaque);
    ZIPentry *entry = finfo->entry;
    void *in = finfo->handle;

    BAIL_IF_MACRO(offset > entry->uncompressed_size, ERR_PAST_EOF, 0);

    if (entry->compression_method == COMPMETH_NONE)
    {
        PHYSFS_sint64 newpos = offset + entry->offset;
        BAIL_IF_MACRO(!__PHYSFS_platformSeek(in, newpos), NULL, 0);
        finfo->uncompressed_position = newpos;
    } /* if */

    else
    {
        /*
         * If seeking backwards, we need to redecode the file
         *  from the start and throw away the compressed bits until we hit
         *  the offset we need. If seeking forward, we still need to
         *  decode, but we don't rewind first.
         */
        if (offset < finfo->uncompressed_position)
        {
            /* we do a copy so state is sane if inflateInit2() fails. */
            z_stream str;
            memset(&str, '\0', sizeof (z_stream));
            if (zlib_err(inflateInit2(&str, -MAX_WBITS)) != Z_OK)
                return(0);

            if (!__PHYSFS_platformSeek(in, entry->offset))
                return(0);

            inflateEnd(&finfo->stream);
            memcpy(&finfo->stream, &str, sizeof (z_stream));
            finfo->uncompressed_position = finfo->compressed_position = 0;
        } /* if */

        while (finfo->uncompressed_position != offset)
        {
            PHYSFS_uint8 buf[512];
            PHYSFS_uint32 maxread = offset - finfo->uncompressed_position;
            if (maxread > sizeof (buf))
                maxread = sizeof (buf);

            if (ZIP_read(handle, buf, maxread, 1) != 1)
                return(0);
        } /* while */
    } /* else */

    return(1);
} /* ZIP_seek */


static PHYSFS_sint64 ZIP_fileLength(FileHandle *handle)
{
    ZIPfileinfo *finfo = (ZIPfileinfo *) (handle->opaque);
    return(finfo->entry->uncompressed_size);
} /* ZIP_fileLength */


static int ZIP_fileClose(FileHandle *handle)
{
    ZIPfileinfo *finfo = (ZIPfileinfo *) (handle->opaque);
    BAIL_IF_MACRO(!__PHYSFS_platformClose(finfo->handle), NULL, 0);

    if (finfo->entry->compression_method != COMPMETH_NONE)
        inflateEnd(&finfo->stream);

    if (finfo->buffer != NULL)
        free(finfo->buffer);

    free(finfo);
    return(1);
} /* ZIP_fileClose */


static PHYSFS_sint64 zip_find_end_of_central_dir(void *in, PHYSFS_sint64 *len)
{
    PHYSFS_uint8 buf[256];
    PHYSFS_sint32 i;
    PHYSFS_sint64 filelen;
    PHYSFS_sint64 filepos;
    PHYSFS_sint32 maxread;
    PHYSFS_sint32 totalread = 0;
    int found = 0;
    PHYSFS_uint32 extra;

    filelen = __PHYSFS_platformFileLength(in);
    BAIL_IF_MACRO(filelen == -1, NULL, 0);

    /*
     * Jump to the end of the file and start reading backwards.
     *  The last thing in the file is the zipfile comment, which is variable
     *  length, and the field that specifies its size is before it in the
     *  file (argh!)...this means that we need to scan backwards until we
     *  hit the end-of-central-dir signature. We can then sanity check that
     *  the comment was as big as it should be to make sure we're in the
     *  right place. The comment length field is 16 bits, so we can stop
     *  searching for that signature after a little more than 64k at most,
     *  and call it a corrupted zipfile.
     */

    if (sizeof (buf) < filelen)
    {
        filepos = filelen - sizeof (buf);
        maxread = sizeof (buf);
    } /* if */
    else
    {
        filepos = 0;
        maxread = filelen;
    } /* else */

    while ((totalread < filelen) && (totalread < 65557))
    {
        BAIL_IF_MACRO(!__PHYSFS_platformSeek(in, filepos), NULL, -1);

        /* make sure we catch a signature between buffers. */
        if (totalread != 0)
        {
            if (__PHYSFS_platformRead(in, buf, maxread - 4, 1) != 1)
                return(-1);
            *((PHYSFS_uint32 *) (&buf[maxread - 4])) = extra;
            totalread += maxread - 4;
        } /* if */
        else
        {
            if (__PHYSFS_platformRead(in, buf, maxread, 1) != 1)
                return(-1);
            totalread += maxread;
        } /* else */

        extra = *((PHYSFS_uint32 *) (&buf[0]));

        for (i = maxread - 4; i > 0; i--)
        {
            if ((buf[i + 0] == 0x50) &&
                (buf[i + 1] == 0x4B) &&
                (buf[i + 2] == 0x05) &&
                (buf[i + 3] == 0x06) )
            {
                found = 1;  /* that's the signature! */
                break;  
            } /* if */
        } /* for */

        if (found)
            break;

        filepos -= (maxread - 4);
    } /* while */

    BAIL_IF_MACRO(!found, ERR_NOT_AN_ARCHIVE, -1);

    if (len != NULL)
        *len = filelen;

    return(filepos + i);
} /* zip_find_end_of_central_dir */


static int ZIP_isArchive(const char *filename, int forWriting)
{
    PHYSFS_uint32 sig;
    int retval;
    void *in;

    in = __PHYSFS_platformOpenRead(filename);
    BAIL_IF_MACRO(in == NULL, NULL, 0);

    /*
     * The first thing in a zip file might be the signature of the
     *  first local file record, so it makes for a quick determination.
     */
    BAIL_IF_MACRO(!readui32(in, &sig), NULL, 0);
    retval = (sig == ZIP_LOCAL_FILE_SIG);
    if (!retval)
    {
        /*
         * No sig...might be a ZIP with data at the start
         *  (a self-extracting executable, etc), so we'll have to do
         *  it the hard way...
         */
        retval = (zip_find_end_of_central_dir(in, NULL) == -1);
    } /* if */

    __PHYSFS_platformClose(in);
    return(retval);
} /* ZIP_isArchive */


static void zip_free_entries(ZIPentry *entries, PHYSFS_uint32 max)
{
    PHYSFS_uint32 i;
    for (i = 0; i < max; i++)
    {
        ZIPentry *entry = &entries[i];
        if (entry->name != NULL)
            free(entry->name);
    } /* for */

    free(entries);
} /* zip_free_entries */


static ZIPentry *zip_find_entry(ZIPinfo *info, const char *path)
{
    ZIPentry *a = info->entries;
    PHYSFS_sint32 lo = 0;
    PHYSFS_sint32 hi = (PHYSFS_sint32) (info->entryCount - 1);
    PHYSFS_sint32 middle;
    int rc;

    while (lo <= hi)
    {
        middle = lo + ((hi - lo) / 2);
        rc = strcmp(path, a[middle].name);
        if (rc == 0)  /* found it! */
            return(&a[middle]);
        else if (rc > 0)
            lo = middle + 1;
        else
            hi = middle - 1;
    } /* while */

    BAIL_MACRO(ERR_NO_SUCH_FILE, NULL);
} /* zip_find_entry */


/* Convert paths from old, buggy DOS zippers... */
static void zip_convert_dos_path(ZIPentry *entry, char *path)
{
    PHYSFS_uint8 hosttype = (PHYSFS_uint8) ((entry->version >> 8) & 0xFF);
    if (hosttype == 0)  /* FS_FAT_ */
    {
        while (*path)
        {
            if (*path == '\\')
                *path = '/';
            path++;
        } /* while */
    } /* if */
} /* zip_convert_dos_path */


static void zip_expand_symlink_path(char *path)
{
    char *ptr = path;
    char *prevptr = path;

    while (1)
    {
        ptr = strchr(ptr, '/');
        if (ptr == NULL)
            break;

        if (*(ptr + 1) == '.')
        {
            if (*(ptr + 2) == '/')
            {
                /* current dir in middle of string: ditch it. */
                memmove(ptr, ptr + 2, strlen(ptr + 2) + 1);
            } /* else if */

            else if (*(ptr + 2) == '\0')
            {
                /* current dir at end of string: ditch it. */
                *ptr = '\0';
            } /* else if */

            else if (*(ptr + 2) == '.')
            {
                if (*(ptr + 3) == '/')
                {
                    /* parent dir in middle: move back one, if possible. */
                    memmove(prevptr, ptr + 4, strlen(ptr + 4) + 1);
                    ptr = prevptr;
                    while (prevptr != path)
                    {
                        prevptr--;
                        if (*prevptr == '/')
                        {
                            prevptr++;
                            break;
                        } /* if */
                    } /* while */
                } /* if */

                if (*(ptr + 3) == '\0')
                {
                    /* parent dir at end: move back one, if possible. */
                    *prevptr = '\0';
                } /* if */
            } /* if */
        } /* if */
        else
        {
            prevptr = ptr;
        } /* else */
    } /* while */
} /* zip_expand_symlink_path */


/*
 * Look for the entry named by (path). If it exists, resolve it, and return
 *  a pointer to that entry. If it's another symlink, keep resolving until you
 *  hit a real file and then return a pointer to the final non-symlink entry.
 *  If there's a problem, return NULL. (path) is always free()'d by this
 *  function.
 */
static ZIPentry *zip_follow_symlink(void *in, ZIPinfo *info, char *path)
{
    ZIPentry *entry;

    zip_expand_symlink_path(path);
    entry = zip_find_entry(info, path);
    if (entry != NULL)
    {
        if (!zip_resolve(in, info, entry))  /* recursive! */
            entry = NULL;
        else
        {
            if (entry->symlink != NULL)
                entry = entry->symlink;
        } /* else */
    } /* if */

    free(path);
    return(entry);
} /* zip_follow_symlink */


static int zip_resolve_symlink(void *in, ZIPinfo *info, ZIPentry *entry)
{
    char *path;
    PHYSFS_uint32 size = entry->uncompressed_size;
    int rc = 0;

    /*
     * We've already parsed the local file header of the symlink at this
     *  point. Now we need to read the actual link from the file data and
     *  follow it.
     */

    BAIL_IF_MACRO(!__PHYSFS_platformSeek(in, entry->offset), NULL, 0);

    path = (char *) malloc(size + 1);
    BAIL_IF_MACRO(path == NULL, ERR_OUT_OF_MEMORY, 0);
    
    if (entry->compression_method == COMPMETH_NONE)
        rc = (__PHYSFS_platformRead(in, path, size, 1) == 1);

    else  /* symlink target path is compressed... */
    {
        z_stream stream;
        PHYSFS_uint32 compsize = entry->compressed_size;
        PHYSFS_uint8 *compressed = (PHYSFS_uint8 *) malloc(compsize);
        if (compressed != NULL)
        {
            if (__PHYSFS_platformRead(in, compressed, compsize, 1) == 1)
            {
                memset(&stream, '\0', sizeof (z_stream));
                stream.next_in = compressed;
                stream.avail_in = compsize;
                stream.next_out = path;
                stream.avail_out = size;
                if (zlib_err(inflateInit2(&stream, -MAX_WBITS)) == Z_OK)
                {
                    rc = zlib_err(inflate(&stream, Z_FINISH));
                    inflateEnd(&stream);

                    /* both are acceptable outcomes... */
                    rc = ((rc == Z_OK) || (rc == Z_STREAM_END));
                } /* if */
            } /* if */
            free(compressed);
        } /* if */
    } /* else */

    if (!rc)
        free(path);
    else
    {
        path[entry->uncompressed_size] = '\0';    /* null-terminate it. */
        zip_convert_dos_path(entry, path);
        entry->symlink = zip_follow_symlink(in, info, path);
    } /* else */

    return(entry->symlink != NULL);
} /* zip_resolve_symlink */


/*
 * Parse the local file header of an entry, and update entry->offset.
 */
static int zip_parse_local(void *in, ZIPentry *entry)
{
    PHYSFS_uint32 ui32;
    PHYSFS_uint16 ui16;
    PHYSFS_uint16 fnamelen;
    PHYSFS_uint16 extralen;

    BAIL_IF_MACRO(!__PHYSFS_platformSeek(in, entry->offset), NULL, 0);
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);
    BAIL_IF_MACRO(ui32 != ZIP_LOCAL_FILE_SIG, ERR_CORRUPTED, 0);
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);
    BAIL_IF_MACRO(ui16 != entry->version_needed, ERR_CORRUPTED, 0);
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);  /* general bits. */
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);
    BAIL_IF_MACRO(ui16 != entry->compression_method, ERR_CORRUPTED, 0);
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);  /* date/time */
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);
    BAIL_IF_MACRO(ui32 != entry->crc, ERR_CORRUPTED, 0);
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);
    BAIL_IF_MACRO(ui32 != entry->compressed_size, ERR_CORRUPTED, 0);
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);
    BAIL_IF_MACRO(ui32 != entry->uncompressed_size, ERR_CORRUPTED, 0);
    BAIL_IF_MACRO(!readui16(in, &fnamelen), NULL, 0);
    BAIL_IF_MACRO(!readui16(in, &extralen), NULL, 0);

    entry->offset += fnamelen + extralen + 30;
    return(1);
} /* zip_parse_local */


static int zip_resolve(void *in, ZIPinfo *info, ZIPentry *entry)
{
    int retval = 1;
    ZipResolveType resolve_type = entry->resolved;

    /* Don't bother if we've failed to resolve this entry before. */
    BAIL_IF_MACRO(resolve_type == ZIP_BROKEN_FILE, ERR_CORRUPTED, 0);
    BAIL_IF_MACRO(resolve_type == ZIP_BROKEN_SYMLINK, ERR_CORRUPTED, 0);

    /* uhoh...infinite symlink loop! */
    BAIL_IF_MACRO(resolve_type == ZIP_RESOLVING, ERR_SYMLINK_LOOP, 0);

    /*
     * We fix up the offset to point to the actual data on the
     *  first open, since we don't want to seek across the whole file on
     *  archive open (can be SLOW on large, CD-stored files), but we
     *  need to check the local file header...not just for corruption,
     *  but since it stores offset info the central directory does not.
     */
    if (resolve_type != ZIP_RESOLVED)
    {
        entry->resolved = ZIP_RESOLVING;

        retval = zip_parse_local(in, entry);
        if (retval)
        {
            /*
             * If it's a symlink, find the original file. This will cause
             *  resolution of other entries (other symlinks and, eventually,
             *  the real file) if all goes well.
             */
            if (resolve_type == ZIP_UNRESOLVED_SYMLINK)
                retval = zip_resolve_symlink(in, info, entry);
        } /* if */

        if (resolve_type == ZIP_UNRESOLVED_SYMLINK)
            entry->resolved = ((retval) ? ZIP_RESOLVED : ZIP_BROKEN_SYMLINK);
        else if (resolve_type == ZIP_UNRESOLVED_FILE)
            entry->resolved = ((retval) ? ZIP_RESOLVED : ZIP_BROKEN_FILE);
    } /* if */

    return(retval);
} /* zip_resolve */


static int zip_version_does_symlinks(PHYSFS_uint32 version)
{
    int retval = 0;
    PHYSFS_uint8 hosttype = (PHYSFS_uint8) ((version >> 8) & 0xFF);

    switch (hosttype)
    {
            /*
             * These are the platforms that can NOT build an archive with
             *  symlinks, according to the Info-ZIP project.
             */
        case 0:  /* FS_FAT_  */
        case 1:  /* AMIGA_   */
        case 2:  /* VMS_     */
        case 4:  /* VM_CSM_  */
        case 6:  /* FS_HPFS_ */
        case 11: /* FS_NTFS_ */
        case 14: /* FS_VFAT_ */
        case 13: /* ACORN_   */
        case 15: /* MVS_     */
        case 18: /* THEOS_   */
            break;  /* do nothing. */

        default:  /* assume the rest to be unix-like. */
            retval = 1;
            break;
    } /* switch */

    return(retval);
} /* zip_version_does_symlinks */


static int zip_entry_is_symlink(ZIPentry *entry)
{
    return((entry->resolved == ZIP_UNRESOLVED_SYMLINK) ||
           (entry->resolved == ZIP_BROKEN_SYMLINK) ||
           (entry->symlink));
} /* zip_entry_is_symlink */


static int zip_has_symlink_attr(ZIPentry *entry, PHYSFS_uint32 extern_attr)
{
    PHYSFS_uint16 xattr = ((extern_attr >> 16) & 0xFFFF);

    return (
              (zip_version_does_symlinks(entry->version)) &&
              (entry->uncompressed_size > 0) &&
              ((xattr & UNIX_FILETYPE_MASK) == UNIX_FILETYPE_SYMLINK)
           );
} /* zip_has_symlink_attr */


PHYSFS_sint64 zip_dos_time_to_physfs_time(PHYSFS_uint32 dostime)
{
    PHYSFS_uint32 dosdate;
    struct tm unixtime;
    memset(&unixtime, '\0', sizeof (unixtime));

    dosdate = (PHYSFS_uint32) ((dostime >> 16) & 0xFFFF);
    dostime &= 0xFFFF;

    /* dissect date */
    unixtime.tm_year = ((dosdate >> 9) & 0x7F) + 80;
    unixtime.tm_mon  = ((dosdate >> 5) & 0x0F) - 1;
    unixtime.tm_mday = ((dosdate     ) & 0x1F);

    /* dissect time */
    unixtime.tm_hour = ((dostime >> 11) & 0x1F);
    unixtime.tm_min  = ((dostime >>  5) & 0x3F);
    unixtime.tm_sec  = ((dostime <<  1) & 0x3E);

    /* let mktime calculate daylight savings time. */
    unixtime.tm_isdst = -1;

    return((PHYSFS_sint64) mktime(&unixtime));
} /* zip_dos_time_to_physfs_time */


static int zip_load_entry(void *in, ZIPentry *entry, PHYSFS_uint32 ofs_fixup)
{
    PHYSFS_uint16 fnamelen, extralen, commentlen;
    PHYSFS_uint32 external_attr;
    PHYSFS_uint16 ui16;
    PHYSFS_uint32 ui32;
    PHYSFS_sint64 si64;

    /* sanity check with central directory signature... */
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);
    BAIL_IF_MACRO(ui32 != ZIP_CENTRAL_DIR_SIG, ERR_CORRUPTED, 0);

    /* Get the pertinent parts of the record... */
    BAIL_IF_MACRO(!readui16(in, &entry->version), NULL, 0);
    BAIL_IF_MACRO(!readui16(in, &entry->version_needed), NULL, 0);
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);  /* general bits */
    BAIL_IF_MACRO(!readui16(in, &entry->compression_method), NULL, 0);
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);
    entry->last_mod_time = zip_dos_time_to_physfs_time(ui32);
    BAIL_IF_MACRO(!readui32(in, &entry->crc), NULL, 0);
    BAIL_IF_MACRO(!readui32(in, &entry->compressed_size), NULL, 0);
    BAIL_IF_MACRO(!readui32(in, &entry->uncompressed_size), NULL, 0);
    BAIL_IF_MACRO(!readui16(in, &fnamelen), NULL, 0);
    BAIL_IF_MACRO(!readui16(in, &extralen), NULL, 0);
    BAIL_IF_MACRO(!readui16(in, &commentlen), NULL, 0);
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);  /* disk number start */
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);  /* internal file attribs */
    BAIL_IF_MACRO(!readui32(in, &external_attr), NULL, 0);
    BAIL_IF_MACRO(!readui32(in, &entry->offset), NULL, 0);
    entry->offset += ofs_fixup;

    entry->symlink = NULL;  /* will be resolved later, if necessary. */
    entry->resolved = (zip_has_symlink_attr(entry, external_attr)) ?
                            ZIP_UNRESOLVED_SYMLINK : ZIP_UNRESOLVED_FILE;

    entry->name = (char *) malloc(fnamelen + 1);
    BAIL_IF_MACRO(entry->name == NULL, ERR_OUT_OF_MEMORY, 0);
    if (__PHYSFS_platformRead(in, entry->name, fnamelen, 1) != 1)
        goto zip_load_entry_puked;

    entry->name[fnamelen] = '\0';  /* null-terminate the filename. */
    zip_convert_dos_path(entry, entry->name);

    si64 = __PHYSFS_platformTell(in);
    if (si64 == -1)
        goto zip_load_entry_puked;

        /* seek to the start of the next entry in the central directory... */
    if (!__PHYSFS_platformSeek(in, si64 + extralen + commentlen))
        goto zip_load_entry_puked;

    return(1);  /* success. */

zip_load_entry_puked:
    free(entry->name);
    return(0);  /* failure. */
} /* zip_load_entry */


static void zip_entry_swap(ZIPentry *a, PHYSFS_uint32 one, PHYSFS_uint32 two)
{
    ZIPentry tmp;
    memcpy(&tmp, &a[one], sizeof (ZIPentry));
    memcpy(&a[one], &a[two], sizeof (ZIPentry));
    memcpy(&a[two], &tmp, sizeof (ZIPentry));
} /* zip_entry_swap */


static void zip_quick_sort(ZIPentry *a, PHYSFS_uint32 lo, PHYSFS_uint32 hi)
{
    PHYSFS_uint32 i;
    PHYSFS_uint32 j;
    ZIPentry *v;

	if ((hi - lo) > ZIP_QUICKSORT_THRESHOLD)
	{
		i = (hi + lo) / 2;

		if (strcmp(a[lo].name, a[i].name) > 0) zip_entry_swap(a, lo, i);
		if (strcmp(a[lo].name, a[hi].name) > 0) zip_entry_swap(a, lo, hi);
		if (strcmp(a[i].name, a[hi].name) > 0) zip_entry_swap(a, i, hi);

		j = hi - 1;
		zip_entry_swap(a, i, j);
		i = lo;
		v = &a[j];
		while (1)
		{
			while(strcmp(a[++i].name, v->name) < 0) {}
			while(strcmp(a[--j].name, v->name) > 0) {}
			if (j < i)
                break;
			zip_entry_swap(a, i, j);
		} /* while */
		zip_entry_swap(a, i, hi-1);
		zip_quick_sort(a, lo, j);
		zip_quick_sort(a, i+1, hi);
	} /* if */
} /* zip_quick_sort */


static void zip_insertion_sort(ZIPentry *a, PHYSFS_uint32 lo, PHYSFS_uint32 hi)
{
    PHYSFS_uint32 i;
    PHYSFS_uint32 j;
    ZIPentry tmp;

    for (i = lo + 1; i <= hi; i++)
    {
        memcpy(&tmp, &a[i], sizeof (ZIPentry));
        j = i;
        while ((j > lo) && (strcmp(a[j - 1].name, tmp.name) > 0))
        {
            memcpy(&a[j], &a[j - 1], sizeof (ZIPentry));
            j--;
        } /* while */
        memcpy(&a[j], &tmp, sizeof (ZIPentry));
    } /* for */
} /* zip_insertion_sort */


static void zip_sort_entries(ZIPentry *entries, PHYSFS_uint32 max)
{
    /*
     * Fast Quicksort algorithm inspired by code from here:
     *   http://www.cs.ubc.ca/spider/harrison/Java/sorting-demo.html
     */
    if (max <= ZIP_QUICKSORT_THRESHOLD)
        zip_quick_sort(entries, 0, max - 1);
	zip_insertion_sort(entries, 0, max - 1);
} /* zip_sort_entries */


static int zip_load_entries(void *in, DirHandle *dirh,
                            PHYSFS_uint32 data_ofs, PHYSFS_uint32 central_ofs)
{
    ZIPinfo *info = (ZIPinfo *) dirh->opaque;
    PHYSFS_uint32 max = info->entryCount;
    PHYSFS_uint32 i;

    BAIL_IF_MACRO(!__PHYSFS_platformSeek(in, central_ofs), NULL, 0);

    info->entries = (ZIPentry *) malloc(sizeof (ZIPentry) * max);
    BAIL_IF_MACRO(info->entries == NULL, ERR_OUT_OF_MEMORY, 0);

    for (i = 0; i < max; i++)
    {
        if (!zip_load_entry(in, &info->entries[i], data_ofs))
        {
            zip_free_entries(info->entries, i);
            return(0);
        } /* if */
    } /* for */

    zip_sort_entries(info->entries, max);
    return(1);
} /* zip_load_entries */


static int zip_parse_end_of_central_dir(void *in, DirHandle *dirh,
                                        PHYSFS_uint32 *data_start,
                                        PHYSFS_uint32 *central_dir_ofs)
{
    ZIPinfo *zipinfo = (ZIPinfo *) dirh->opaque;
    PHYSFS_uint32 ui32;
    PHYSFS_uint16 ui16;
    PHYSFS_sint64 len;
    PHYSFS_sint64 pos;

    /* find the end-of-central-dir record, and seek to it. */
    pos = zip_find_end_of_central_dir(in, &len);
    BAIL_IF_MACRO(pos == -1, NULL, 0);
	BAIL_IF_MACRO(!__PHYSFS_platformSeek(in, pos), NULL, 0);

    /* check signature again, just in case. */
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);
    BAIL_IF_MACRO(ui32 != ZIP_END_OF_CENTRAL_DIR_SIG, ERR_NOT_AN_ARCHIVE, 0);

	/* number of this disk */
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);
    BAIL_IF_MACRO(ui16 != 0, ERR_UNSUPPORTED_ARCHIVE, 0);

	/* number of the disk with the start of the central directory */
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);
    BAIL_IF_MACRO(ui16 != 0, ERR_UNSUPPORTED_ARCHIVE, 0);

	/* total number of entries in the central dir on this disk */
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);

	/* total number of entries in the central dir */
    BAIL_IF_MACRO(!readui16(in, &zipinfo->entryCount), NULL, 0);
    BAIL_IF_MACRO(ui16 != zipinfo->entryCount, ERR_UNSUPPORTED_ARCHIVE, 0);

	/* size of the central directory */
    BAIL_IF_MACRO(!readui32(in, &ui32), NULL, 0);

	/* offset of central directory */
    BAIL_IF_MACRO(!readui32(in, central_dir_ofs), NULL, 0);
    BAIL_IF_MACRO(pos < *central_dir_ofs + ui32, ERR_UNSUPPORTED_ARCHIVE, 0);

    /*
     * For self-extracting archives, etc, there's crapola in the file
     *  before the zipfile records; we calculate how much data there is
     *  prepended by determining how far the central directory offset is
     *  from where it is supposed to be (start of end-of-central-dir minus
     *  sizeof central dir)...the difference in bytes is how much arbitrary
     *  data is at the start of the physical file.
     */
	*data_start = pos - (*central_dir_ofs + ui32);

    /* Now that we know the difference, fix up the central dir offset... */
    *central_dir_ofs += *data_start;

	/* zipfile comment length */
    BAIL_IF_MACRO(!readui16(in, &ui16), NULL, 0);

    /*
     * Make sure that the comment length matches to the end of file...
     *  If it doesn't, we're either in the wrong part of the file, or the
     *  file is corrupted, but we give up either way.
     */
    BAIL_IF_MACRO((pos + 22 + ui16) != len, ERR_UNSUPPORTED_ARCHIVE, 0);

    return(1);  /* made it. */
} /* zip_parse_end_of_central_dir */


static DirHandle *zip_allocate_dirhandle(const char *name)
{
    char *ptr;
    ZIPinfo *info;
    DirHandle *retval = malloc(sizeof (DirHandle));
    BAIL_IF_MACRO(retval == NULL, ERR_OUT_OF_MEMORY, NULL);

    memset(retval, '\0', sizeof (DirHandle));

    info = (ZIPinfo *) malloc(sizeof (ZIPinfo));
    if (info == NULL)
    {
        free(retval);
        BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
    } /* if */

    memset(info, '\0', sizeof (ZIPinfo));

    ptr = (char *) malloc(strlen(name) + 1);
    if (ptr == NULL)
    {
        free(info);
        free(retval);
        BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
    } /* if */

    info->archiveName = ptr;
    strcpy(info->archiveName, name);
    retval->opaque = info;
    retval->funcs = &__PHYSFS_DirFunctions_ZIP;
    return(retval);
} /* zip_allocate_dirhandle */


static DirHandle *ZIP_openArchive(const char *name, int forWriting)
{
    DirHandle *retval = NULL;
    void *in = NULL;
    PHYSFS_uint32 data_start;
    PHYSFS_uint32 cent_dir_ofs;
    int success = 0;

    BAIL_IF_MACRO(forWriting, ERR_ARC_IS_READ_ONLY, NULL);

    if ((in = __PHYSFS_platformOpenRead(name)) == NULL)
        goto zip_openarchive_end;
    
    if ((retval = zip_allocate_dirhandle(name)) == NULL)
        goto zip_openarchive_end;

    if (!zip_parse_end_of_central_dir(in, retval, &data_start, &cent_dir_ofs))
        goto zip_openarchive_end;

    if (!zip_load_entries(in, retval, data_start, cent_dir_ofs))
        goto zip_openarchive_end;

    success = 1;  /* ...and we're good to go.  :)  */

zip_openarchive_end:
    if (!success)  /* clean up for failures. */
    {
        if (retval != NULL)
        {
            if (retval->opaque != NULL)
            {
                if (((ZIPinfo *) (retval->opaque))->archiveName != NULL)
                    free(((ZIPinfo *) (retval->opaque))->archiveName);

                free(retval->opaque);
            } /* if */

            free(retval);
            retval = NULL;
        } /* if */
    } /* if */

    if (in != NULL)
        __PHYSFS_platformClose(in);  /* Close this even with success. */

    return(retval);
} /* ZIP_openArchive */


static PHYSFS_sint32 zip_find_start_of_dir(ZIPinfo *info, const char *path,
                                            int stop_on_first_find)
{
    PHYSFS_sint32 lo = 0;
    PHYSFS_sint32 hi = (PHYSFS_sint32) info->entryCount;
    PHYSFS_sint32 middle;
    PHYSFS_uint32 dlen = strlen(path);
    PHYSFS_sint32 retval = -1;
    const char *name;
    int rc;

    if (*path == '\0')  /* root dir? */
        return(0);

    if ((dlen > 0) && (path[dlen - 1] == '/')) /* ignore trailing slash. */
        dlen--;

    while (lo <= hi)
    {
        middle = lo + ((hi - lo) / 2);
        name = info->entries[middle].name;
        rc = strncmp(path, name, dlen);
        if (rc == 0)
        {
            char ch = name[dlen];
            if (ch < '/') /* make sure this isn't just a substr match. */
                rc = -1;
            else if (ch > '/')
                rc = 1;
            else 
            {
                if ((name[dlen + 1] == '\0') || (stop_on_first_find))
                    return(middle);

                /* there might be more entries earlier in the list. */
                retval = middle;
                hi = middle - 1;
            } /* else */
        } /* if */

        if (rc > 0)
            lo = middle + 1;
        else
            hi = middle - 1;
    } /* while */

    return(retval);
} /* zip_find_start_of_dir */


static LinkedStringList *ZIP_enumerateFiles(DirHandle *h,
                                            const char *dirname,
                                            int omitSymLinks)
{
    ZIPinfo *info = ((ZIPinfo *) h->opaque);
    PHYSFS_sint32 i, max = (PHYSFS_sint32) info->entryCount;
    LinkedStringList *retval = NULL, *p = NULL;
    PHYSFS_uint32 dlen = strlen(dirname);

    if (dirname[dlen - 1] == '/')
        dlen--;

    i = zip_find_start_of_dir(info, dirname, 0);
    BAIL_IF_MACRO(i == -1, ERR_NO_SUCH_FILE, NULL);

    while (1)
    {
        ZIPentry *entry = &info->entries[i];
        const char *add_file;
        size_t strsize;
        char *slash;

        add_file = entry->name + dlen + ((dlen > 0) ? 1 : 0);
        if ( ((omitSymLinks) && (zip_entry_is_symlink(entry))) ||
             (*add_file == '\0') ) /* skip links and the dir entry itself. */
        {
            i++;
            continue;
        } /* if */

        slash = strchr(add_file, '/'); /* handle subdirs under dirname... */
        strsize = (size_t) ((slash) ? (slash - add_file) : strlen(add_file));

        retval = __PHYSFS_addToLinkedStringList(retval, &p, add_file, strsize);

        if (++i >= max)
            break; /* we're at the end of the entries array. */

        if ((dlen > 0) && (strncmp(info->entries[i].name, dirname, dlen) != 0))
            break; /* we're past this dir's entries. */

        /* We added a subdir? Skip its children. */
        while (slash != NULL)
        {
            if (strncmp(info->entries[i].name, dirname, dlen) == 0)
            {
                if (info->entries[i].name[dlen] == '/')
                {
                    i++;  /* skip it. */
                    continue;
                } /* if */
            } /* if */
            slash = NULL;
        } /* while */
    } /* while */

    return(retval);
} /* ZIP_enumerateFiles */


static int ZIP_exists(DirHandle *h, const char *name)
{
    ZIPentry *entry = zip_find_entry((ZIPinfo *) h->opaque, name);
    return(entry != NULL);
} /* ZIP_exists */


static PHYSFS_sint64 ZIP_getLastModTime(DirHandle *h, const char *name)
{
    ZIPentry *entry = zip_find_entry((ZIPinfo *) h->opaque, name);
    BAIL_IF_MACRO(entry == NULL, NULL, -1);
    return(entry->last_mod_time);
} /* ZIP_getLastModTime */


static int ZIP_isDirectory(DirHandle *h, const char *name)
{
    ZIPinfo *info = (ZIPinfo *) h->opaque;
    PHYSFS_uint32 pos;
    ZIPentry *entry;

    pos = zip_find_start_of_dir(info, name, 1);
    if (pos >= 0)
        return(1); /* definitely a dir. */

    /* Follow symlinks. This means we might need to resolve entries. */
    entry = zip_find_entry(info, name);
    BAIL_IF_MACRO(entry == NULL, ERR_NO_SUCH_FILE, 0);

    if (entry->resolved == ZIP_UNRESOLVED_SYMLINK) /* gotta resolve it. */
    {
        int rc;
        void *in = __PHYSFS_platformOpenRead(info->archiveName);
        BAIL_IF_MACRO(in == NULL, NULL, 0);
        rc = zip_resolve(in, info, entry);
        __PHYSFS_platformClose(in);
        if (!rc)
            return(0);
    } /* if */

    BAIL_IF_MACRO(entry->resolved == ZIP_BROKEN_SYMLINK, NULL, 0);
    BAIL_IF_MACRO(entry->symlink == NULL, ERR_NOT_A_DIR, 0);

    return(zip_find_start_of_dir(info, entry->symlink->name, 1) >= 0);
} /* ZIP_isDirectory */


static int ZIP_isSymLink(DirHandle *h, const char *name)
{
    ZIPentry *entry = zip_find_entry((ZIPinfo *) h->opaque, name);
    BAIL_IF_MACRO(entry == NULL, NULL, 0);
    return(zip_entry_is_symlink(entry));
} /* ZIP_isSymLink */


static void *zip_get_file_handle(const char *fn, ZIPinfo *inf, ZIPentry *entry)
{
    int success;
    void *retval = __PHYSFS_platformOpenRead(fn);
    BAIL_IF_MACRO(retval == NULL, NULL, NULL);

    success = zip_resolve(retval, inf, entry);
    if (success)
    {
        PHYSFS_sint64 offset;
        offset = ((entry->symlink) ? entry->symlink->offset : entry->offset);
        success = __PHYSFS_platformSeek(retval, offset);
    } /* if */

    if (!success)
    {
        __PHYSFS_platformClose(retval);
        retval = NULL;
    } /* if */

    return(retval);
} /* zip_get_file_handle */


static FileHandle *ZIP_openRead(DirHandle *h, const char *filename)
{
    ZIPinfo *info = (ZIPinfo *) h->opaque;
    ZIPentry *entry = zip_find_entry(info, filename);
    FileHandle *retval = NULL;
    ZIPfileinfo *finfo = NULL;
    void *in;

    BAIL_IF_MACRO(entry == NULL, NULL, NULL);

    in = zip_get_file_handle(info->archiveName, info, entry);
    BAIL_IF_MACRO(in == NULL, NULL, NULL);

    if ( ((retval = (FileHandle *) malloc(sizeof (FileHandle))) == NULL) ||
         ((finfo = (ZIPfileinfo *) malloc(sizeof (ZIPfileinfo))) == NULL) )
    {
        if (retval)
            free(retval);
        __PHYSFS_platformClose(in);
        BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
    } /* if */

    retval->opaque = (void *) finfo;
    retval->funcs = &__PHYSFS_FileFunctions_ZIP;
    retval->dirHandle = h;

    memset(finfo, '\0', sizeof (ZIPfileinfo));
    finfo->handle = in;
    finfo->entry = ((entry->symlink != NULL) ? entry->symlink : entry);
    if (finfo->entry->compression_method != COMPMETH_NONE)
    {
        if (zlib_err(inflateInit2(&finfo->stream, -MAX_WBITS)) != Z_OK)
        {
            ZIP_fileClose(retval);
            return(NULL);
        } /* if */

        finfo->buffer = (PHYSFS_uint8 *) malloc(ZIP_READBUFSIZE);
        if (finfo->buffer == NULL)
        {
            ZIP_fileClose(retval);
            BAIL_MACRO(ERR_OUT_OF_MEMORY, NULL);
        } /* if */
    } /* if */

    return(retval);
} /* ZIP_openRead */


static void ZIP_dirClose(DirHandle *h)
{
    ZIPinfo *zi = (ZIPinfo *) (h->opaque);
    zip_free_entries(zi->entries, zi->entryCount);
    free(zi->entries);
    free(zi->archiveName);
    free(zi);
    free(h);
} /* ZIP_dirClose */

#endif  /* defined PHYSFS_SUPPORTS_ZIP */

/* end of zip.c ... */

