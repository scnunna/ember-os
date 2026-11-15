package com.nunna.embercompanion.android

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.nunna.embercompanion.link.LinkController
import com.nunna.embercompanion.link.LinkTransport
import java.util.UUID

/**
 * BLE glue: connects to the watch, asks for MTU 247 and 2M PHY, subscribes to EVT and AUDIO, queues CMD writes,
 * and reconnects with a growing delay. All protocol logic lives in [LinkController].
 * Needs the BLUETOOTH_CONNECT permission (the service checks it before calling [start]).
 */
@SuppressLint("MissingPermission")
class EmberGatt(private val ctx: Context, private val link: () -> LinkController) : LinkTransport {
    companion object {
        val SERVICE: UUID = UUID.fromString("eb3f0001-6d62-4f53-8a1e-656d6265720a")
        val CMD: UUID = UUID.fromString("eb3f0002-6d62-4f53-8a1e-656d6265720a")
        val EVT: UUID = UUID.fromString("eb3f0003-6d62-4f53-8a1e-656d6265720a")
        val AUDIO: UUID = UUID.fromString("eb3f0004-6d62-4f53-8a1e-656d6265720a")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val TAG = "EmberGatt"
        val BACKOFF_MS = longArrayOf(1_000, 2_000, 4_000, 8_000, 15_000, 30_000, 60_000)
    }

    private val main = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var cmd: BluetoothGattCharacteristic? = null
    private var address: String? = null
    private var wanted = false
    private var attempt = 0
    private var mtu = 23
    private val queue = ArrayDeque<ByteArray>()
    private var writing = false
    private var subscribeStep = 0

    /** Last human-readable problem, or null. */
    @Volatile var lastError: String? = null
        private set

    fun start(addr: String) {
        main.post {
            address = addr
            wanted = true
            attempt = 0
            connectNow()
        }
    }

    fun stop() {
        main.post {
            wanted = false
            main.removeCallbacksAndMessages(null)
            closeGatt()
            link().onDisconnected()
        }
    }

    private fun adapter(): BluetoothAdapter? = (ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private fun connectNow() {
        if (!wanted) return
        val a = adapter()
        if (a == null || !a.isEnabled) {
            lastError = Errors.BT_OFF
            scheduleReconnect()
            return
        }
        val addr = address ?: return
        closeGatt()
        link().onConnecting()
        try {
            val dev = a.getRemoteDevice(addr)
            gatt = dev.connectGatt(ctx, false, callback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: Exception) {
            Log.w(TAG, "connect failed", e)
            lastError = Errors.LINK_LOST
            scheduleReconnect()
        }
    }

    private fun closeGatt() {
        queue.clear(); writing = false; cmd = null
        gatt?.let { runCatching { it.disconnect() }; runCatching { it.close() } }
        gatt = null
    }

    private fun scheduleReconnect() {
        if (!wanted) return
        val delay = BACKOFF_MS[minOf(attempt, BACKOFF_MS.size - 1)]
        attempt++
        main.postDelayed({ connectNow() }, delay)
    }

    override fun write(packets: List<ByteArray>) {
        main.post {
            queue.addAll(packets)
            pump()
        }
    }

    private fun pump() {
        if (writing) return
        val g = gatt ?: return
        val c = cmd ?: return
        val next = queue.removeFirstOrNull() ?: return
        writing = true
        val ok = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, next, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION") run {
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                c.value = next
                g.writeCharacteristic(c)
            }
        }
        if (!ok) { writing = false; Log.w(TAG, "write rejected") }
    }

    private fun subscribeNext(g: BluetoothGatt) {
        val svc = g.getService(SERVICE) ?: run {
            lastError = Errors.BT_NO_WATCH
            closeGatt(); scheduleReconnect(); return
        }
        val uuid = when (subscribeStep) { 0 -> EVT; 1 -> AUDIO; else -> null }
        if (uuid == null) {
            attempt = 0
            lastError = null
            link().onReady(mtu)
            return
        }
        val ch = svc.getCharacteristic(uuid)
        if (ch == null) { subscribeStep++; subscribeNext(g); return }
        g.setCharacteristicNotification(ch, true)
        val d = ch.getDescriptor(CCCD)
        if (d == null) { subscribeStep++; subscribeNext(g); return }
        val ok = if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION") run { d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; g.writeDescriptor(d) }
        }
        if (!ok) { Log.w(TAG, "cccd write rejected"); subscribeStep++; subscribeNext(g) }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            main.post {
                if (g !== gatt) return@post
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    mtu = 23
                    if (!g.requestMtu(247)) g.discoverServices()
                } else {
                    link().onDisconnected()
                    lastError = Errors.LINK_LOST
                    closeGatt()
                    scheduleReconnect()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            main.post {
                if (g !== gatt) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                runCatching { g.setPreferredPhy(BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_LE_2M_MASK, BluetoothDevice.PHY_OPTION_NO_PREFERRED) }
                g.discoverServices()
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            main.post {
                if (g !== gatt) return@post
                val svc = g.getService(SERVICE)
                cmd = svc?.getCharacteristic(CMD)
                if (status != BluetoothGatt.GATT_SUCCESS || svc == null || cmd == null) {
                    lastError = Errors.BT_NO_WATCH
                    closeGatt(); scheduleReconnect(); return@post
                }
                subscribeStep = 0
                subscribeNext(g)
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            main.post {
                if (g !== gatt) return@post
                subscribeStep++
                subscribeNext(g)
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            main.post {
                writing = false
                if (status != BluetoothGatt.GATT_SUCCESS) Log.w(TAG, "write status $status")
                pump()
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            deliver(c.uuid, value)
        }

        @Deprecated("Android 12/12L callback")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) @Suppress("DEPRECATION") deliver(c.uuid, c.value ?: return)
        }
    }

    private fun deliver(uuid: UUID, value: ByteArray) {
        when (uuid) {
            EVT -> link().onEvtPacket(value)
            AUDIO -> link().onAudioPacket(value)
        }
    }
}
