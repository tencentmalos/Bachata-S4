package com.shadps4.android

import android.content.Context
import android.content.ContextWrapper
import android.net.Uri
import android.provider.DocumentsContract
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import com.shadps4.android.data.*
import com.shadps4.android.database.*
import com.shadps4.android.runtime.input.NativePad
import com.shadps4.android.runtime.session.NativeFexSession
import java.io.File
import java.nio.file.Files
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith

/** Optional actual producer/consumer acceptance. Only generated PS4 fixtures are executed. */
@RunWith(AndroidJUnit4::class)
class ManagedRomInstrumentedTest {
    private fun context():Context {
        val target=InstrumentationRegistry.getInstrumentation().targetContext
        return object:ContextWrapper(target){
            override fun getFilesDir()=File(target.filesDir,"validation/emupsp-contract").apply{mkdirs()}
            override fun getSharedPreferences(name:String,mode:Int)=target.getSharedPreferences("emupsp-contract-$name",mode)
        }
    }
    @Test fun consumesProducerMountsBaseUpdateDlcAndExecutesWithoutCopying()=runBlocking {
        assumeTrue(InstrumentationRegistry.getArguments().getString("managed_contract")=="consume")
        val context=context();assertEquals(0L,NativeFexSession.nativeCurrentGeneration())
        val tree=DocumentsContract.buildTreeDocumentUri(ManagedRomContract.AUTHORITY,"platform:ps4")
        EmuPspLibrary.connect(context,tree)
        val sync=EmuPspLibrary.sync(context);assertNull(sync.error)
        val dir=File(context.filesDir,"games/emupsp-CUSA99999")
        val link=requireNotNull(EmuPspLibrary.readLink(dir))
        assertTrue(link.available);assertEquals(setOf("base","update","dlc"),link.game.components.map{it.kind}.toSet())
        assertTrue(GameInstallVerifier.canLaunch(context.filesDir,"games/${dir.name}"))
        assertTrue(dir.walkTopDown().filter{it.isFile&&!Files.isSymbolicLink(it.toPath())}.sumOf{it.length()}<65536)
        assertFalse(dir.walkTopDown().any{it.extension=="pkg"||it.name=="eboot.bin"})
        val keep=File(context.filesDir,"saves/keep").apply{parentFile!!.mkdirs();writeText("preserve-save")}
        val entity=GameEntity(dir.name,"Synthetic package","games/${dir.name}",tree.toString(),0)
        val dao=OneGameDao(entity);val repository=GameRepository(dao,context)
        assertEquals("emupsp",repository.getGame(dir.name)!!.sourceOwner)
        val deletion=runCatching{repository.deleteGame(dir.name)}
        assertTrue(deletion.exceptionOrNull() is IllegalStateException);assertNotNull(dao.getById(dir.name));assertTrue(dir.isDirectory)
        val file=DocumentsContract.buildDocumentUriUsingTree(tree,link.game.components.first().documentId)
        assertTrue(runCatching{context.contentResolver.openFileDescriptor(file,"rw")!!.close()}.isFailure)
        assertTrue(runCatching{DocumentsContract.deleteDocument(context.contentResolver,file)}.isFailure)
        context.contentResolver.query(file,arrayOf(DocumentsContract.Document.COLUMN_FLAGS),null,null,null)!!.use{assertTrue(it.moveToFirst());assertEquals(0,it.getInt(0) and DocumentsContract.Document.FLAG_SUPPORTS_DELETE)}
        assertTrue(NativePad.nativeInitializeHost(File(context.filesDir,"host").path))
        EmuPspLibrary.open(context,dir).use{lease->
            assertTrue(Files.isSymbolicLink(lease.executable.toPath()))
            assertEquals(3,lease.executable.parentFile!!.walkTopDown().count{Files.isSymbolicLink(it.toPath())})
            val effective=NativeFexSession.nativeInspectArchive(lease.executable.path)!!
            assertTrue(String(effective[0],Charsets.ISO_8859_1).contains("01.06"))
            val generation=NativeFexSession.nativeStartExecutable("CUSA99999",lease.executable.path);assertTrue(generation>0)
            try{
                val outcome=NativeFexSession.nativeWaitTerminal(generation,20000)
                val detail=NativeFexSession.nativeTerminalDetail(generation).orEmpty()
                android.util.Log.i("ManagedRomAcceptance","owner=emupsp components=3 payload_copied=0 deletion_blocked=true outcome=$outcome $detail")
                assertEquals(detail,NativeFexSession.Outcome.RETURNED,outcome);assertTrue(detail,detail.startsWith("guest return=51966"))
            }finally{NativeFexSession.nativeRequestStop(generation,1000)}
        }
        assertEquals("preserve-save",keep.readText())
        assertTrue(File(context.filesDir,"rom-leases").listFiles().orEmpty().isEmpty())
        // Re-read from a new consumer pass: permissions and component identities persist.
        assertNull(EmuPspLibrary.sync(context).error);assertEquals(link.game.fingerprint,EmuPspLibrary.readLink(dir)!!.game.fingerprint)
    }
    @Test fun sourceRemovalDropsOnlyReferenceAndRetainsSaves() {
        assumeTrue(InstrumentationRegistry.getArguments().getString("managed_contract")=="removed")
        val context=context();val keep=File(context.filesDir,"saves/keep")
        try{
            assertTrue(keep.isFile)
            val sync=EmuPspLibrary.sync(context);assertNull(sync.error)
            assertTrue(sync.removed.contains("emupsp-CUSA99999"));assertFalse(File(context.filesDir,"games/emupsp-CUSA99999").exists())
            assertEquals("preserve-save",keep.readText())
            reconcileRealLibrary()
            android.util.Log.i("ManagedRomAcceptance","source_removed=true reference_removed=true saves_retained=true")
        }finally{
            if(NativeFexSession.nativeCurrentGeneration()==0L)context.filesDir.deleteRecursively()
            context.getSharedPreferences("managed-rom-library",Context.MODE_PRIVATE).edit().clear().commit()
        }
    }
    @Test fun cleansOwnedConsumerRegistration() {
        assumeTrue(InstrumentationRegistry.getArguments().getString("managed_contract")=="cleanup")
        reconcileRealLibrary()
    }
    private fun reconcileRealLibrary()=runBlocking {
        val target=InstrumentationRegistry.getInstrumentation().targetContext
        val repository=dagger.hilt.android.EntryPointAccessors.fromApplication(target.applicationContext,BachataNavEntryPoint::class.java).gameRepository()
        repository.syncLibrary()
        assertNull(repository.getGame("emupsp-CUSA99999"))
    }
    private class OneGameDao(private var value:GameEntity?):GameDao {
        override suspend fun insert(game:GameEntity){value=game}
        override fun observeAll():Flow<List<GameEntity>> = flowOf(listOfNotNull(value))
        override suspend fun getAll()=listOfNotNull(value)
        override suspend fun getById(id:String)=value?.takeIf{it.id==id}
        override suspend fun updateTitle(id:String,title:String)=0
        override suspend fun updateMetadata(id:String,title:String,subtitle:String?,detail:String?,importedAtMs:Long)=0
        override suspend fun updateLastLaunched(id:String,timestamp:Long)=0
        override suspend fun deleteById(id:String):Int {value=null;return 1}
    }
}
