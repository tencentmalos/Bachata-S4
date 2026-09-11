package com.shadps4.android.di

import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import com.shadps4.android.BuildConfig
import com.shadps4.android.feature.setup.DownloadRuntime

@Module
@InstallIn(SingletonComponent::class)
object AppModule {
    @Provides
    @DownloadRuntime
    fun provideDownloadRuntime(): Boolean = BuildConfig.DOWNLOAD_RUNTIME
}
