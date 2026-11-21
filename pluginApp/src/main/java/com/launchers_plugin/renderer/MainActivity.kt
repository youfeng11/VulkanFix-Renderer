package com.launchers_plugin.renderer

import android.app.Activity
import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Process
import android.os.RemoteException
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject

class MainActivity : Activity() {
    private lateinit var wrapperMode: Button
    private lateinit var nativeMode: Button
    private lateinit var startButton: Button
    private lateinit var copyButton: Button
    private lateinit var deviceName: TextView
    private lateinit var deviceApi: TextView
    private lateinit var resultState: TextView
    private lateinit var resultTitle: TextView
    private lateinit var resultDetail: TextView
    private lateinit var groups: LinearLayout
    private lateinit var scroll: ScrollView
    private lateinit var progress: ProgressBar
    private lateinit var surfaceContainer: FrameLayout
    private lateinit var preview: SurfaceView
    private val handler = Handler(Looper.getMainLooper())
    private val preferences by lazy { getSharedPreferences("driver_post", Context.MODE_PRIVATE) }
    private var selectedWrapper = true
    private var worker: Messenger? = null
    private var workerPid = 0
    private var bound = false
    private var running = false
    private var waitingForSurface = false
    private var wrapped = false
    private var runId = 0
    private val partialReport = StringBuilder()
    private var completedRows = 0
    private var lastResult = ""
    private val timeout = Runnable { fail("连续 120 秒没有新的结果，检测已停止。") }

    private val receiver = Messenger(Handler(Looper.getMainLooper()) { message ->
        if (running && message.arg2 == runId) when (message.what) {
            DriverPostService.HELLO -> {
                workerPid = message.arg1
                awaitProgress()
            }
            DriverPostService.PROGRESS -> {
                val line = message.data.getString("row").orEmpty()
                partialReport.append(line)
                completedRows++
                lastResult = try { JSONObject(line).optString("name") } catch (_: Exception) { "" }
                showProgress()
                awaitProgress()
            }
            DriverPostService.RESULT -> {
                val report = message.data.getString("report").orEmpty()
                if (message.arg1 == 1) partialReport.append(report)
                else partialReport.clear().append(report)
                finishWorker()
                renderReport()
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
                    arg2 = runId
                    replyTo = receiver
                    data = Bundle().apply { putParcelable("surface", preview.holder.surface) }
                })
            } catch (_: RemoteException) { fail("无法启动检测。") }
        }
        override fun onServiceDisconnected(name: ComponentName) {
            if (running) fail("驱动检测进程异常退出，已保存收到的结果。")
        }
        override fun onNullBinding(name: ComponentName) { if (running) fail("无法连接检测服务。") }
        override fun onBindingDied(name: ComponentName) { if (running) fail("检测服务连接已失效。") }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        wrapperMode = findViewById(R.id.mode_wrapper)
        nativeMode = findViewById(R.id.mode_native)
        startButton = findViewById(R.id.start_probe)
        copyButton = findViewById(R.id.copy_report)
        deviceName = findViewById(R.id.device_name)
        deviceApi = findViewById(R.id.device_api)
        resultState = findViewById(R.id.result_state)
        resultTitle = findViewById(R.id.result_title)
        resultDetail = findViewById(R.id.result_detail)
        groups = findViewById(R.id.check_groups)
        scroll = findViewById(R.id.report_scroll)
        progress = findViewById(R.id.probe_progress)
        surfaceContainer = findViewById(R.id.present_surface)
        preview = createPreview()
        surfaceContainer.addView(preview)
        findViewById<View>(R.id.result_summary).background = rounded(color(R.color.panel_background), 16, color(R.color.panel_border))
        findViewById<View>(R.id.mode_switch).background = rounded(color(R.color.panel_border), 13)
        startButton.background = ripple(color(R.color.accent), 14)
        selectedWrapper = preferences.getBoolean("selected_wrapper", true)
        wrapperMode.setOnClickListener { selectMode(true) }
        nativeMode.setOnClickListener { selectMode(false) }
        startButton.setOnClickListener {
            if (running) fail("检测已停止，已保存收到的结果。") else startProbe()
        }
        copyButton.setOnClickListener {
            val report = listOf(false, true).joinToString("\n") { useWrapper ->
                "${if (useWrapper) "wrapper" else "native"}\n${rawReport(useWrapper)}"
            }
            copyText(report)
        }
        renderReport()
    }

    private fun selectMode(useWrapper: Boolean) {
        if (running || selectedWrapper == useWrapper) return
        selectedWrapper = useWrapper
        preferences.edit().putBoolean("selected_wrapper", useWrapper).apply()
        renderReport()
    }

    private fun rawReport(useWrapper: Boolean): String {
        if (running && wrapped == useWrapper) return partialReport.toString()
        return preferences.getString(if (useWrapper) "wrapper" else "native", "").orEmpty()
    }

    private fun renderReport() {
        val report = DriverReport(rawReport(selectedWrapper))
        wrapperMode.background = ripple(color(if (selectedWrapper) R.color.panel_background else R.color.panel_border), 10)
        nativeMode.background = ripple(color(if (selectedWrapper) R.color.panel_border else R.color.panel_background), 10)
        wrapperMode.setTextColor(color(if (selectedWrapper) R.color.accent else R.color.text_secondary))
        nativeMode.setTextColor(color(if (selectedWrapper) R.color.text_secondary else R.color.accent))
        wrapperMode.isSelected = selectedWrapper
        nativeMode.isSelected = !selectedWrapper
        wrapperMode.isEnabled = !running
        nativeMode.isEnabled = !running
        wrapperMode.alpha = if (running) 0.55f else 1f
        nativeMode.alpha = wrapperMode.alpha
        startButton.text = if (running) "停止检测" else if (report.rows.isEmpty()) "开始检测" else "重新检测"
        copyButton.isEnabled = rawReport(false).isNotBlank() || rawReport(true).isNotBlank()
        copyButton.alpha = if (copyButton.isEnabled) 1f else 0.4f
        deviceName.text = report.deviceName.ifEmpty { Build.MODEL }
        deviceApi.text = if (report.apiVersion.isEmpty()) "" else "Vulkan ${report.apiVersion}"
        deviceApi.visibility = if (report.apiVersion.isEmpty()) View.GONE else View.VISIBLE
        resultTitle.setTextColor(color(R.color.text_primary))
        groups.removeAllViews()
        if (running) {
            showProgress()
        } else if (report.rows.isEmpty()) {
            resultState.text = "尚未检测"
            resultTitle.text = "检查驱动兼容性"
            resultDetail.text = "开始检测后，这里会显示执行结果。"
        } else {
            resultState.text = if (report.complete) "自检已完成" else "上次检测未完成"
            resultTitle.text = when {
                !report.complete -> "检测中断"
                report.failures.isNotEmpty() -> "${report.failures.size} 项执行异常"
                else -> "未发现执行异常"
            }
            if (report.failures.isNotEmpty()) resultTitle.setTextColor(color(R.color.result_fail))
            resultDetail.text = "通过 ${report.passed.size} 项 · 兼容提示 ${report.warnings.size} 项 · 未测试 ${report.skipped.size} 项"
            addGroup("执行异常", report.failures, expanded = true)
            addGroup("兼容提示", report.warnings)
            addGroup("通过的检查", report.passed)
            addGroup("未测试项目", report.skipped)
            addGroup("功能支持", report.capabilities)
        }
        scroll.post { scroll.scrollTo(0, 0) }
    }

    private fun showProgress() {
        resultState.text = "正在检测${if (wrapped) " VulkanFix" else "原生驱动"}"
        resultTitle.text = if (completedRows == 0) "准备检测…" else "已返回 $completedRows 项结果"
        resultDetail.text = if (lastResult.isEmpty()) "正在连接检测服务…" else "最近完成 · ${checkTitle(lastResult)}"
        copyButton.isEnabled = partialReport.isNotEmpty() || rawReport(!wrapped).isNotBlank()
        copyButton.alpha = if (copyButton.isEnabled) 1f else 0.4f
    }

    private fun addGroup(title: String, checks: List<DriverCheck>, expanded: Boolean = false) {
        if (checks.isEmpty()) return
        val panel = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = rounded(color(R.color.panel_background), 14, color(R.color.panel_border))
        }
        val header = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(16))
            minimumHeight = dp(56)
            isFocusable = true
            background = ripple(Color.TRANSPARENT, 14)
        }
        header.addView(label("$title · ${checks.size}", 15f), LinearLayout.LayoutParams(0, -2, 1f))
        val indicator = label(if (expanded) "−" else "+", 20f, color(R.color.text_secondary))
        header.addView(indicator)
        panel.addView(header)
        val body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            visibility = if (expanded) View.VISIBLE else View.GONE
        }
        fun populate() {
            for (check in checks) body.addView(checkView(check))
        }
        if (expanded) populate()
        panel.addView(body)
        header.contentDescription = "$title，${checks.size} 项，${if (expanded) "收起" else "展开"}"
        header.setOnClickListener {
            val open = body.visibility != View.VISIBLE
            if (open && body.childCount == 0) populate()
            body.visibility = if (open) View.VISIBLE else View.GONE
            indicator.text = if (open) "−" else "+"
            header.contentDescription = "$title，${checks.size} 项，${if (open) "收起" else "展开"}"
        }
        groups.addView(panel, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(12) })
    }

    private fun checkView(check: DriverCheck): View {
        val outer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(14), dp(16), dp(14))
            minimumHeight = dp(56)
            background = ripple(Color.TRANSPARENT, 10)
            isFocusable = true
            contentDescription = "${check.title}，${check.statusText}，查看诊断"
            setOnClickListener { showCheck(check) }
        }
        val row = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        row.addView(label(check.title, 14f), LinearLayout.LayoutParams(0, -2, 1f).apply { marginEnd = dp(12) })
        val stateColor = when {
            check.evidence == "query" -> R.color.text_secondary
            check.status == "FAIL" -> R.color.result_fail
            check.status == "WARN" -> R.color.result_warning
            check.status == "PASS" -> R.color.result_pass
            else -> R.color.text_secondary
        }
        row.addView(label(check.statusText, 12f, color(stateColor)))
        row.addView(label("›", 20f, color(R.color.text_secondary)),
            LinearLayout.LayoutParams(-2, -2).apply { marginStart = dp(10) })
        outer.addView(row)
        if (check.name == "fillModeNonSolid.line" && check.status == "FAIL") {
            outer.addView(label("线框绘制的像素读回不符合预期。", 13f, color(R.color.text_secondary)).apply {
                setPadding(0, dp(8), 0, 0)
            })
        }
        return outer
    }

    private fun showCheck(check: DriverCheck) {
        val diagnostic = "${check.name}\n\n${check.detail}"
        val body = label(diagnostic, 13f).apply {
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
            setPadding(dp(24), dp(16), dp(24), dp(16))
        }
        AlertDialog.Builder(this).setTitle("${check.title} · ${check.statusText}")
            .setView(ScrollView(this).apply { addView(body) })
            .setPositiveButton("关闭", null)
            .setNeutralButton("复制此项") { _, _ -> copyText(diagnostic) }
            .show()
    }

    private fun copyText(value: String) {
        (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
            .setPrimaryClip(ClipData.newPlainText("VulkanFix 驱动自检", value))
        Toast.makeText(this, "报告已复制", Toast.LENGTH_SHORT).show()
    }

    private fun startProbe() {
        running = true
        runId++
        wrapped = selectedWrapper
        partialReport.clear()
        completedRows = 0
        lastResult = ""
        waitingForSurface = true
        progress.visibility = View.VISIBLE
        awaitProgress()
        renderReport()
        preview = createPreview()
        surfaceContainer.removeAllViews()
        surfaceContainer.addView(preview)
        surfaceContainer.visibility = View.VISIBLE
    }

    private fun awaitProgress() {
        handler.removeCallbacks(timeout)
        handler.postDelayed(timeout, 120_000)
    }

    private fun createPreview(): SurfaceView = SurfaceView(this).apply {
        contentDescription = "画面呈现检测"
        holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                if (running && waitingForSurface && holder === preview.holder) {
                    waitingForSurface = false
                    bindProbe()
                }
            }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) = Unit
            override fun surfaceDestroyed(holder: SurfaceHolder) {
                if (running && holder === preview.holder) fail("检测窗口已关闭。")
            }
        })
    }

    private fun bindProbe() {
        try {
            bound = bindService(Intent(this, DriverPostService::class.java), connection, Context.BIND_AUTO_CREATE)
            if (!bound) fail("无法启动检测服务。")
        } catch (error: Exception) { fail("无法启动检测，${error.message}") }
    }

    private fun finishWorker() {
        if (running && partialReport.isNotEmpty()) {
            preferences.edit().putString(if (wrapped) "wrapper" else "native", partialReport.toString()).apply()
        }
        running = false
        waitingForSurface = false
        handler.removeCallbacks(timeout)
        if (worker?.binder?.isBinderAlive == true && workerPid > 0 && workerPid != Process.myPid())
            Process.killProcess(workerPid)
        if (bound) unbindService(connection)
        bound = false
        worker = null
        workerPid = 0
        surfaceContainer.visibility = View.GONE
        progress.visibility = View.GONE
    }

    private fun fail(message: String) {
        partialReport.append(JSONObject().put("name", "probe").put("status", "FAIL")
            .put("evidence", "error").put("detail",
                if (lastResult.isEmpty()) message else "$message 最近完成 ${checkTitle(lastResult)}。")
            .toString()).append('\n')
        finishWorker()
        renderReport()
    }

    private fun label(value: String, size: Float, textColor: Int = color(R.color.text_primary)) = TextView(this).apply {
        text = value
        textSize = size
        setTextColor(textColor)
    }

    private fun color(resource: Int) = getColor(resource)
    private fun dp(value: Int) = (value * resources.displayMetrics.density + 0.5f).toInt()
    private fun rounded(fill: Int, radius: Int, border: Int = Color.TRANSPARENT) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = dp(radius).toFloat()
        setStroke(dp(1), border)
    }
    private fun ripple(fill: Int, radius: Int) = RippleDrawable(
        ColorStateList.valueOf(color(R.color.accent_surface)),
        rounded(fill, radius),
        rounded(Color.WHITE, radius)
    )

    override fun onDestroy() {
        finishWorker()
        super.onDestroy()
    }
}
