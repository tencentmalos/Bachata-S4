package com.shadps4.android.data

import android.content.ContentResolver
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants

/** Own a seekable source FD without retaining ContentResolver's stable provider reference. */
fun ContentResolver.openIndependentPkgFd(uri: Uri): ParcelFileDescriptor =
    (openFileDescriptor(uri, "r") ?: error("Cannot reopen PKG for extract")).use { source ->
        // The source may have changed since probing. A pipe still needs the cache-copy path.
        Os.lseek(source.fileDescriptor, 0, OsConstants.SEEK_CUR)
        // Keeping the resolver wrapper during extraction makes Android kill this app
        // if the provider dies. A duplicated regular FD survives the provider process;
        // proxy-backed descriptors may still report an I/O error, handled by the importer.
        ParcelFileDescriptor.dup(source.fileDescriptor)
    }
