package com.launchers_plugin.renderer

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Process
import android.os.RemoteException
import android.view.Surface
import androidx.annotation.Keep
import org.json.JSONObject

@Keep
internal object DriverPostNative {
    init { System.loadLibrary("vulkanfix_post") }
    external fun run(library: String, surface: Surface?): String
}

// Private, disposable process: never load a probed ICD into the UI or game.
class DriverPostService : Service() {
    companion object {
        const val START = 1
        const val HELLO = 2
        const val RESULT = 3
        const val STOP = 4
    }

    private var started = false
    private val messenger = Messenger(Handler(Looper.getMainLooper()) { message ->
        when (message.what) {
            STOP -> Process.killProcess(Process.myPid())
            START -> if (!started && message.replyTo != null) {
                started = true
                val receiver = message.replyTo
                val wrapped = message.arg1 == 1
                @Suppress("DEPRECATION")
                val surface = message.data.getParcelable<Surface>("surface")
                try {
                    receiver.send(Message.obtain(null, HELLO).apply { arg1 = Process.myPid() })
                } catch (_: RemoteException) {
                    Process.killProcess(Process.myPid())
                }
                Thread({
                    val report = try {
                        DriverPostNative.run(if (wrapped) "${applicationInfo.nativeLibraryDir}/libvulkanfix.so" else "libvulkan.so", surface)
                    } catch (error: Throwable) {
                        JSONObject().put("name", "probe").put("status", "FAIL")
                            .put("evidence", "error").put("detail", error.toString()).toString() + "\n"
                    } finally {
                        surface?.release()
                    }
                    try {
                        receiver.send(Message.obtain(null, RESULT).apply {
                            data = Bundle().apply { putString("report", report) }
                        })
                    } catch (_: RemoteException) {
                        Process.killProcess(Process.myPid())
                    }
                }, "VulkanFixDriverPost").start()
            }
        }
        true
    })

    override fun onBind(intent: Intent): IBinder = messenger.binder
    override fun onUnbind(intent: Intent): Boolean {
        Process.killProcess(Process.myPid())
        return false
    }
}
