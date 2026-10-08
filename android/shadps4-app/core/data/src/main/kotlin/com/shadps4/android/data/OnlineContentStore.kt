package com.shadps4.android.data

import android.content.Context
import java.io.File
import org.json.JSONObject
import spatial.content.OnlineContent

/** Host policy only. Login, background scheduling and PKG mounting remain app-owned. */
object OnlineContentStore {
    fun open(context: Context, verifiedCategory: Long = 0, archiveTool: File? = null): OnlineContent =
        OnlineContent(OnlineContent.configuration(
            context = context,
            root = File(context.filesDir, "online-content/ps4"),
            platform = "ps4",
            baseUrl = "https://2468c.com",
            category = verifiedCategory,
            cloudRoot = "/ShadPS4OnlineContent",
            archiveTool = archiveTool
        ))

    fun planShare(service: OnlineContent, shareUrl: String, title: String): Boolean =
        service.command(JSONObject().put("op", "plan_share").put("link", shareUrl).put("title", title))
}
