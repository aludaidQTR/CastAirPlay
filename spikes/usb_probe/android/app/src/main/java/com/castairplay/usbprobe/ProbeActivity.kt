package com.castairplay.usbprobe

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.net.Socket
import kotlin.concurrent.thread

class ProbeActivity : Activity() {
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        status = TextView(this).apply { text = "Ready to test adb reverse." }
        val button = Button(this).apply {
            text = "Send probe"
            setOnClickListener { sendProbe() }
        }
        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
            addView(status)
            addView(button)
        })
    }

    private fun sendProbe() {
        status.text = "Connecting to PC..."
        thread(name = "aicam-usb-probe") {
            val result = runCatching {
                Socket("127.0.0.1", 47800).use { socket ->
                    socket.soTimeout = 10_000
                    socket.getOutputStream().write(
                        "AICAM_USB_PROBE Android\n".toByteArray(Charsets.UTF_8)
                    )
                    socket.getOutputStream().flush()
                    val reply = socket.getInputStream()
                        .bufferedReader(Charsets.UTF_8).readLine()
                    check(reply == "ACK Android") {
                        "Unexpected PC response: $reply"
                    }
                    "PASS: $reply"
                }
            }.getOrElse { "FAIL: ${it.message}" }
            runOnUiThread { status.text = result }
        }
    }
}
