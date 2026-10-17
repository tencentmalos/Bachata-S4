package com.shadps4.android.data

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.system.Os
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File
import java.util.UUID
import kotlinx.serialization.json.*

/** Owns references and small UI metadata. ROM bytes are opened through read-only SAF FDs. */
object EmuPspLibrary {
    const val MODE="emupsp-reference"
    const val LINK="emupsp.link.json"
    data class Link(val tree:String,val game:ManagedRomContract.Game,val available:Boolean)
    data class SyncResult(val linked:Int=0,val removed:List<String> = emptyList(),val error:String?=null)
    fun isSource(uri:String)=runCatching{Uri.parse(uri).authority==ManagedRomContract.AUTHORITY}.getOrDefault(false)
    private fun tree(context:Context)=context.getSharedPreferences("managed-rom-library",Context.MODE_PRIVATE).getString("tree",null)
    fun validateTree(uri:Uri){
        require(uri.scheme=="content"&&uri.authority==ManagedRomContract.AUTHORITY&&uri.query==null&&uri.fragment==null&&DocumentsContract.isTreeUri(uri)) {"Choose the EmuPSP PS4 library"}
        require(DocumentsContract.getTreeDocumentId(uri) in setOf("library","platform:ps4")){"Choose the PS4 library"}
    }
    fun connect(context:Context,uri:Uri){
        validateTree(uri)
        context.contentResolver.takePersistableUriPermission(uri,Intent.FLAG_GRANT_READ_URI_PERMISSION)
        android.util.Log.i("ManagedRom","Persistent read-only grant acquired")
        readGames(context,uri) // Validate before replacing a previously working connection.
        check(context.getSharedPreferences("managed-rom-library",Context.MODE_PRIVATE).edit().putString("tree",uri.toString()).commit())
    }
    private fun readGames(context:Context,tree:Uri):List<ManagedRomContract.Game>{
        validateTree(tree)
        val document=DocumentsContract.buildDocumentUriUsingTree(tree,"catalog:ps4")
        val bytes=context.contentResolver.openInputStream(document)?.use{input->
            val out=java.io.ByteArrayOutputStream();val buf=ByteArray(8192)
            while(true){val n=input.read(buf);if(n<0)break;require(out.size()+n<=8*1024*1024){"Library manifest too large"};out.write(buf,0,n)};out.toByteArray()
        }?:error("EmuPSP library unavailable")
        return ManagedRomContract.parse(bytes.toString(Charsets.UTF_8))
    }
    fun readLink(dir:File):Link?=runCatching{
        val file=File(dir,LINK);if(!file.isFile||file.length()>8*1024*1024)return null
        val v=ManagedRomContract.objectFrom(file.readText());val tree=ManagedRomContract.string(v,"tree")
        validateTree(Uri.parse(tree))
        val root=buildJsonObject{put("format",ManagedRomContract.FORMAT);put("schema",1);put("owner","emupsp");put("read_only",true);put("platform","ps4");put("games",buildJsonArray{add(v.getValue("game"))})}
        Link(tree,ManagedRomContract.parse(root.toString()).single(),v["available"]?.jsonPrimitive?.booleanOrNull==true)
    }.getOrNull()
    fun managed(dir:File)=File(dir,LINK).exists()||InstallManifestIo.read(dir)?.mode==MODE
    fun describe(dir:File):String? {
        val link=readLink(dir)?:return null
        return link.game.components.joinToString("\n") { file ->
            val identity=file.raw["identity"]!!.jsonObject
            val origin=file.raw["origin"]?.jsonObject?:buildJsonObject{}
            listOf(file.kind,ManagedRomContract.string(identity,"display_version"),
                ManagedRomContract.string(origin,"kind"),ManagedRomContract.string(origin,"provider"),
                ManagedRomContract.string(origin,"source_url")).filter{it.isNotBlank()}.joinToString(" · ")
        }
    }
    private fun saveLink(dir:File,uri:Uri,game:ManagedRomContract.Game,available:Boolean){
        val value=buildJsonObject{put("schema",1);put("owner","emupsp");put("tree",uri.toString());put("game",game.raw);put("available",available)}
        val tmp=File(dir,"$LINK.tmp");tmp.writeText(value.toString());Os.rename(tmp.path,File(dir,LINK).path)
    }
    private fun directory(context:Context,id:String):File{
        require(Regex("CUSA[0-9]{5}").matches(id))
        val games=File(context.filesDir,"games").canonicalFile
        val dir=File(games,"emupsp-$id")
        require(!java.nio.file.Files.isSymbolicLink(dir.toPath())&&dir.canonicalFile.parentFile==games){"Invalid reference directory"}
        return dir
    }
    fun sync(context:Context):SyncResult {
        val configured=tree(context)?:return SyncResult()
        val uri=Uri.parse(configured)
        val games=try{readGames(context,uri)}catch(e:Exception){
            File(context.filesDir,"games").listFiles().orEmpty().filter{it.isDirectory&&managed(it)}.forEach{dir->readLink(dir)?.let{saveLink(dir,Uri.parse(it.tree),it.game,false)}}
            return SyncResult(error="EmuPSP unavailable or permission lost; reconnect its PS4 library")
        }
        var linked=0;val keep=hashSetOf<String>();val errors=mutableListOf<String>()
        for(game in games){
            val dir=directory(context,game.titleId);keep+=dir.name
            val hasBase=game.components.count{it.kind=="base"}==1
            if(!hasBase){if(managed(dir))saveLink(dir,uri,game,false);continue}
            try{
                if(dir.exists()&&!managed(dir))error("Reference location already contains installed content")
                val previous=readLink(dir)
                if(previous?.game?.fingerprint!=game.fingerprint||!File(dir,"sce_sys/param.sfo").isFile){
                    open(context,uri,game).use{lease->
                        val metadata=requireNotNull(NativeFexSession.nativeInspectArchive(lease.executable.path)){"PKG mount validation failed"}
                        require(metadata.size==2&&ParamSfoReader.parse(metadata[0]).titleId==game.titleId){"PKG title differs from library"}
                        val cache=File(dir,"sce_sys");cache.mkdirs();File(cache,"param.sfo").writeBytes(metadata[0]);if(metadata[1].isNotEmpty())File(cache,"icon0.png").writeBytes(metadata[1])
                    }
                }
                saveLink(dir,uri,game,true)
                InstallManifestIo.write(dir,InstallManifest(status=InstallManifestIo.STATUS_INSTALLED,gameId=dir.name,contentId=game.components.first{it.kind=="base"}.contentId,
                    mode=MODE,sourceUri=uri.toString(),installedAtMs=InstallManifestIo.read(dir)?.installedAtMs?:System.currentTimeMillis(),requiredFiles=listOf(LINK,"sce_sys/param.sfo"),bytesTotal=game.components.sumOf{it.size}))
                linked++
            }catch(e:Exception){if(managed(dir))saveLink(dir,uri,game,false);errors+="${game.titleId}: ${e.message}"}
        }
        val removed=mutableListOf<String>()
        for(dir in File(context.filesDir,"games").listFiles().orEmpty()){
            if(dir.isDirectory&&managed(dir)&&dir.name !in keep&&dir.canonicalFile.parentFile==File(context.filesDir,"games").canonicalFile){
                if(dir.deleteRecursively())removed+=dir.name
            }
        }
        android.util.Log.i("ManagedRom","sync linked=$linked removed=${removed.size} failures=${errors.size}; owner=emupsp payload_copied=0")
        return SyncResult(linked,removed,errors.firstOrNull())
    }
    class Lease internal constructor(val executable:File,private val directory:File,private val files:List<ParcelFileDescriptor>):AutoCloseable{
        override fun close(){files.forEach{runCatching{it.close()}};directory.deleteRecursively()}
    }
    fun open(context:Context,dir:File):Lease{
        val link=readLink(dir)?:error("EmuPSP reference is invalid; reconnect the library")
        val current=readGames(context,Uri.parse(link.tree)).firstOrNull{it.id==link.game.id}?:error("Game removed in EmuPSP")
        return open(context,Uri.parse(link.tree),current)
    }
    private fun open(context:Context,tree:Uri,game:ManagedRomContract.Game):Lease{
        val base=game.components.singleOrNull{it.kind=="base"}?:error("Choose a base game in EmuPSP")
        val issues=game.raw["issues"]?.jsonArray.orEmpty().map{it.jsonPrimitive.content}
        require(issues.none{it in setOf("same_version_variants","chosen_variant_missing","content_id_mismatch","required_version_missing")}){"Resolve component choices in EmuPSP"}
        val leaseRoot=File(context.filesDir,"rom-leases").canonicalFile;leaseRoot.mkdirs()
        val directory=File(leaseRoot,UUID.randomUUID().toString());check(directory.mkdir())
        val files=mutableListOf<ParcelFileDescriptor>()
        try{
            for(file in game.components){
                val uri=DocumentsContract.buildDocumentUriUsingTree(tree,file.documentId)
                val fd=context.contentResolver.openIndependentPkgFd(uri)
                files+=fd
                require(fd.statSize==file.size){"Source package size changed; refresh EmuPSP"}
                val path=when(file.kind){"base"->File(directory,"${game.titleId}.pkg");"update"->File(directory,"${game.titleId}-UPD.pkg");else->File(directory,"DLC/${file.id}.pkg")}
                path.parentFile!!.mkdirs();Os.symlink("/proc/self/fd/${fd.fd}",path.path)
            }
            android.util.Log.i("ManagedRom","open ${game.titleId} components=${files.size} owner=emupsp payload_copied=0")
            return Lease(File(directory,"${base.titleId}.pkg"),directory,files)
        }catch(e:Throwable){files.forEach{runCatching{it.close()}};directory.deleteRecursively();throw e}
    }
}
