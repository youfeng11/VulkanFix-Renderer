package com.launchers_plugin.renderer

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Process
import android.os.RemoteException
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONObject

class MainActivity : Activity() {
    private lateinit var content: LinearLayout
    private lateinit var status: TextView
    private lateinit var results: LinearLayout
    private lateinit var preview: SurfaceView
    private lateinit var root: LinearLayout
    private lateinit var scroll: ScrollView
    private var waitingForSurface = false
    private val buttons = mutableListOf<Button>()
    private val handler = Handler(Looper.getMainLooper())
    private var worker: Messenger? = null
    private var workerPid = 0
    private var bound = false
    private var running = false
    private var wrapped = false
    private val preferences by lazy { getSharedPreferences("driver_post", Context.MODE_PRIVATE) }
    private val timeout = Runnable { fail("自检超时，已结束探测进程。") }
    private val receiver = Messenger(Handler(Looper.getMainLooper()) { message ->
        if (running) when (message.what) {
            DriverPostService.HELLO -> workerPid = message.arg1
            DriverPostService.RESULT -> {
                val report = message.data.getString("report") ?: ""
                preferences.edit().putString(if (wrapped) "wrapper" else "native", report).apply()
                status.text = if (hasCompletion(report)) "自检已结束；查询与实际执行结果分别列出。" else "自检中止，以下保留已返回的诊断。"
                finishWorker()
                renderReports()
            }
        }
        true
    })
    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            if (!running) return
            worker = Messenger(binder)
            try {
                worker?.send(Message.obtain(null, DriverPostService.START).apply {
                    arg1 = if (wrapped) 1 else 0
                    replyTo = receiver
                    data = Bundle().apply { putParcelable("surface", preview.holder.surface) }
                })
            } catch (_: RemoteException) { fail("无法启动探测进程。") }
        }
        override fun onServiceDisconnected(name: ComponentName) {
            if (running) fail("驱动探测进程异常退出，未完成本次自检。")
        }
        override fun onNullBinding(name: ComponentName) { if (running) fail("无法连接探测服务。") }
        override fun onBindingDied(name: ComponentName) { if (running) fail("探测服务连接已失效。") }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val padding = (16 * resources.displayMetrics.density).toInt()
            setPadding(padding, padding, padding, padding)
        }
        root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        scroll = ScrollView(this).apply { addView(content) }
        root.addView(scroll,
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        preview = createPreview()
        root.addView(preview, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
            (48 * resources.displayMetrics.density).toInt()))
        setContentView(root)
        text("VulkanFix 驱动自检", 22f)
        text("分别检测原生驱动与 VulkanFix。查询通过表示声明支持，执行通过表示对应探针的结果正确。", 14f)
        for ((title, useWrapper) in listOf("检测原生驱动" to false, "检测 VulkanFix" to true)) {
            val button = Button(this).apply { this.text = title; setOnClickListener { startProbe(useWrapper) } }
            content.addView(button)
            buttons += button
        }
        content.addView(Button(this).apply {
            text = "复制报告"
            setOnClickListener {
                val report = listOf("native", "wrapper").joinToString("\n") { key ->
                    "$key\n${preferences.getString(key, "尚未检测")}"
                }
                (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
                    .setPrimaryClip(ClipData.newPlainText("VulkanFix Driver POST", report))
                status.text = "报告已复制。"
            }
        })
        status = text("尚未开始本次自检。", 14f)
        results = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        content.addView(results)
        renderReports()
    }

    private fun text(value: String, size: Float): TextView = TextView(this).apply {
        text = value; textSize = size; setPadding(0, 8, 0, 8); content.addView(this)
    }

    private fun startProbe(useWrapper: Boolean) {
        if (running) return
        running = true
        wrapped = useWrapper
        waitingForSurface = true
        buttons.forEach { it.isEnabled = false }
        status.text = "正在检测${if (wrapped) " VulkanFix" else "原生驱动"}…"
        handler.postDelayed(timeout, 30_000)
        // The previous one-shot worker may have retained WSI objects until
        // exit. Never reconnect a second ICD to that old BufferQueue.
        val oldPreview = preview
        preview = createPreview()
        root.removeView(oldPreview)
        root.addView(preview, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
            (48 * resources.displayMetrics.density).toInt()))
    }

    private fun createPreview(): SurfaceView = SurfaceView(this).apply {
        contentDescription = "交换链呈现测试区域"
        holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                if (running && waitingForSurface && holder === preview.holder) {
                    waitingForSurface = false
                    bindProbe()
                }
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                if (running && holder === preview.holder) fail("测试窗口已关闭，本次自检已停止。")
            }
        })
    }

    private fun bindProbe() {
        try {
            bound = bindService(Intent(this, DriverPostService::class.java), connection, Context.BIND_AUTO_CREATE)
            if (!bound) fail("无法启动探测服务。")
        } catch (error: Exception) { fail("无法启动自检：${error.message}") }
    }

    private fun finishWorker() {
        running = false
        waitingForSurface = false
        handler.removeCallbacks(timeout)
        // The PID came from our private service. Never target a game process.
        if (worker?.binder?.isBinderAlive == true && workerPid > 0 && workerPid != Process.myPid())
            Process.killProcess(workerPid)
        if (bound) unbindService(connection)
        bound = false
        worker = null
        workerPid = 0
        buttons.forEach { it.isEnabled = true }
    }

    private fun fail(message: String) {
        status.text = message
        finishWorker()
    }

    private fun hasCompletion(report: String): Boolean = report.lineSequence().any { line ->
        try { JSONObject(line).optString("evidence") == "completion" } catch (_: Exception) { false }
    }

    private fun renderReports() {
        results.removeAllViews()
        for ((key, title) in listOf("native" to "原生驱动（上次结果）", "wrapper" to "VulkanFix（上次结果）")) {
            results.addView(TextView(this).apply { text = title; textSize = 18f; setPadding(0, 24, 0, 8) })
            val report = preferences.getString(key, null) ?: continue
            for (line in report.lineSequence().filter { it.isNotBlank() }) {
                results.addView(TextView(this).apply {
                    // Selectable TextViews steal focus during report rebuild
                    // and can scroll the action buttons off screen. The copy
                    // button exports the complete report without that jump.
                    textSize = 13f; setPadding(0, 6, 0, 6)
                    try {
                        val row = JSONObject(line)
                        val evidence = when (row.optString("evidence")) {
                            "query" -> "查询"; "functional" -> "执行"; "error" -> "错误"; else -> "信息"
                        }
                        val verdict = row.optString("status")
                        text = "[$evidence · $verdict] ${row.optString("name")}\n${row.optString("detail")}"
                        setTextColor(when (verdict) {
                            "FAIL" -> Color.rgb(185, 40, 40)
                            "PASS" -> Color.rgb(20, 115, 55)
                            "WARN" -> Color.rgb(150, 90, 0)
                            else -> Color.DKGRAY
                        })
                    } catch (_: Exception) { text = "报告行无法解析：$line" }
                })
            }
        }
        scroll.post { scroll.fullScroll(View.FOCUS_UP) }
    }

    override fun onDestroy() {
        finishWorker()
        super.onDestroy()
    }
}
