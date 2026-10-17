package com.shadps4.android.data

import kotlinx.serialization.json.*

/** Public, emulator-neutral EmuPSP contract. Package facts remain independently verified at mount. */
object ManagedRomContract {
    const val FORMAT="EmuPSP.ReadOnlyLibrary"
    const val AUTHORITY="io.github.tencentmalos.emupsp.documents"
    const val ACTION="io.github.tencentmalos.content.OPEN_LIBRARY"
    const val MIME="application/vnd.emupsp.library+json"
    private val json=Json { ignoreUnknownKeys=true }
    data class Component(val id:String,val documentId:String,val name:String,val size:Long,val sha256:String,
                         val kind:String,val titleId:String,val contentId:String,val raw:JsonObject)
    data class Game(val id:String,val title:String,val titleId:String,val components:List<Component>,val raw:JsonObject) {
        val fingerprint:String get()=components.joinToString("|"){it.id+":"+it.sha256}
    }
    fun objectFrom(text:String):JsonObject=json.parseToJsonElement(text).jsonObject
    fun string(v:JsonObject,key:String)=v[key]?.jsonPrimitive?.contentOrNull.orEmpty()
    fun parse(text:String):List<Game> {
        require(text.length<=8*1024*1024){"Library manifest too large"}
        val root=objectFrom(text)
        require(string(root,"format")==FORMAT&&root["schema"]?.jsonPrimitive?.intOrNull==1&&
            string(root,"owner")=="emupsp"&&root["read_only"]?.jsonPrimitive?.booleanOrNull==true&&string(root,"platform")=="ps4"){"Unsupported managed library"}
        val games=root["games"]?.jsonArray?:error("Missing games")
        require(games.size<=5000)
        val seen=hashSetOf<String>()
        return games.map { value->
            val g=value.jsonObject;val titleId=string(g,"title_id")
            require(Regex("CUSA[0-9]{5}").matches(titleId)&&string(g,"id")=="ps4:$titleId"&&seen.add(titleId)){"Invalid or duplicate game identity"}
            val entries=g["components"]?.jsonArray?:error("Missing components");require(entries.size<=5000)
            val ids=hashSetOf<String>()
            val components=entries.filter{it.jsonObject["selected"]?.jsonPrimitive?.booleanOrNull==true}.map { item->
                val f=item.jsonObject;val identity=f["identity"]?.jsonObject?:error("Missing package facts")
                val doc=string(f,"document_id");val sha=string(f,"sha256");val id=string(f,"id")
                require(Regex("[a-f0-9]{64}").matches(id)&&ids.add(id)&&Regex("[a-fA-F0-9]{64}").matches(sha)){"Invalid component receipt"}
                // SAF document IDs are opaque. Authorization is enforced by the
                // selected tree and the producer, never by assuming its private layout.
                require(doc.isNotBlank() && doc.length<=4096 && doc.none { it.isISOControl() }) {"Invalid document handle"}
                val size=f["size"]?.jsonPrimitive?.longOrNull?:0L
                require(size>0&&string(f,"name").endsWith(".pkg",true)&&string(identity,"status")=="identified"&&
                    string(identity,"base_title_id")==titleId&&string(identity,"title_id")==titleId){"Package identity is not verified"}
                val kind=string(identity,"kind");require(kind in setOf("base","update","dlc"))
                Component(id,doc,string(f,"name"),size,sha.lowercase(),kind,titleId,string(identity,"content_id"),f)
            }
            val base=components.filter{it.kind=="base"};val updates=components.filter{it.kind=="update"}
            require(base.size<=1&&updates.size<=1){"Ambiguous base/update selection"}
            require(components.filter{it.kind=="dlc"}.let{list->list.map{it.contentId}.distinct().size==list.size}){"Duplicate DLC selection"}
            if(base.isNotEmpty()&&updates.isNotEmpty())require(base[0].contentId==updates[0].contentId){"Update Content ID differs from base"}
            Game(string(g,"id"),string(g,"title").take(300),titleId,components,g)
        }
    }
}
