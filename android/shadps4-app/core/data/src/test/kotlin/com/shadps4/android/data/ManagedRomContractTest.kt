package com.shadps4.android.data

import java.util.Base64
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test

class ManagedRomContractTest {
    private fun component(kind:String="base",name:String="game.pkg",path:String="ps4/baidu/library/job/content/game.pkg",content:String="UP0000-CUSA99999_00-SYNTHETIC0000000")=buildJsonObject{
        put("id",if(kind=="base")"a".repeat(64)else"b".repeat(64));put("document_id","file:"+Base64.getUrlEncoder().withoutPadding().encodeToString(path.toByteArray()))
        put("name",name);put("size",1234);put("sha256","c".repeat(64));put("selected",true)
        put("identity",buildJsonObject{put("status","identified");put("title_id","CUSA99999");put("base_title_id","CUSA99999");put("kind",kind);put("content_id",content)})
        put("origin",buildJsonObject{put("provider","baidu");put("source_url","https://2468c.com/test")})
    }
    private fun manifest(vararg components:JsonObject)=buildJsonObject{
        put("format",ManagedRomContract.FORMAT);put("schema",1);put("owner","emupsp");put("platform","ps4");put("read_only",true)
        put("games",buildJsonArray{add(buildJsonObject{put("id","ps4:CUSA99999");put("title_id","CUSA99999");put("title","Test");put("components",JsonArray(components.toList()))})})
    }.toString()
    @Test fun readsStableIdentitySelectedComponentsAndPublicOrigin(){
        val game=ManagedRomContract.parse(manifest(component(),component("update"))).single()
        assertEquals("ps4:CUSA99999",game.id);assertEquals(listOf("base","update"),game.components.map{it.kind})
        assertEquals("https://2468c.com/test",game.components[0].raw["origin"]!!.jsonObject["source_url"]!!.jsonPrimitive.content)
    }
    @Test fun rejectsInvalidHandlesAndWrongTitle() {
        for(doc in listOf("", "x".repeat(4097), "bad\nhandle")){
            val f=JsonObject(component().toMutableMap().apply{put("document_id",JsonPrimitive(doc))})
            assertThrows(IllegalArgumentException::class.java){ManagedRomContract.parse(manifest(f))}
        }
        val f=component();val identity=JsonObject(f.getValue("identity").jsonObject.toMutableMap().apply{put("title_id",JsonPrimitive("CUSA00001"))})
        assertThrows(IllegalArgumentException::class.java){ManagedRomContract.parse(manifest(JsonObject(f.toMutableMap().apply{put("identity",identity)})))}
    }
    @Test fun rejectsSchemaChangesAmbiguousVariantsAndMismatchedUpdates(){
        assertThrows(IllegalArgumentException::class.java){ManagedRomContract.parse(manifest(component()).replace("\"schema\":1","\"schema\":2"))}
        assertThrows(IllegalArgumentException::class.java){ManagedRomContract.parse(manifest(component(),component()))}
        assertThrows(IllegalArgumentException::class.java){ManagedRomContract.parse(manifest(component(),component("update",content="UP0000-CUSA99999_00-OTHER00000000000")))}
    }
}
