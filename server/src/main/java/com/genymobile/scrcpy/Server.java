package com.genymobile.scrcpy;

import android.os.BatteryManager;
import android.os.Build;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

public final class Server {

    private Server() {
        // not instantiable
    }

    private static void initAndCleanUp(Options options) {
        boolean mustDisableShowTouchesOnCleanUp = false;
        int restoreStayOn = -1;
        boolean restoreNormalPowerMode = options.getControl(); // only restore power mode if control is enabled
        if (options.getShowTouches() || options.getStayAwake()) {
            if (options.getShowTouches()) {
                try {
                    String oldValue = Settings.getAndPutValue(Settings.TABLE_SYSTEM, "show_touches", "1");
                    // If "show touches" was disabled, it must be disabled back on clean up
                    mustDisableShowTouchesOnCleanUp = !"1".equals(oldValue);
                } catch (SettingsException e) {
                    Ln.e("Could not change \"show_touches\"", e);
                }
            }

            if (options.getStayAwake()) {
                int stayOn = BatteryManager.BATTERY_PLUGGED_AC | BatteryManager.BATTERY_PLUGGED_USB | BatteryManager.BATTERY_PLUGGED_WIRELESS;
                try {
                    String oldValue = Settings.getAndPutValue(Settings.TABLE_GLOBAL, "stay_on_while_plugged_in", String.valueOf(stayOn));
                    try {
                        restoreStayOn = Integer.parseInt(oldValue);
                        if (restoreStayOn == stayOn) {
                            // No need to restore
                            restoreStayOn = -1;
                        }
                    } catch (NumberFormatException e) {
                        restoreStayOn = 0;
                    }
                } catch (SettingsException e) {
                    Ln.e("Could not change \"stay_on_while_plugged_in\"", e);
                }
            }
        }

        if (options.getCleanup()) {
            try {
                CleanUp.configure(options.getDisplayId(), restoreStayOn, mustDisableShowTouchesOnCleanUp, restoreNormalPowerMode,
                        options.getPowerOffScreenOnClose());
            } catch (IOException e) {
                Ln.e("Could not configure cleanup", e);
            }
        }
    }

    private static void scrcpy(Options options) throws IOException, ConfigurationException {
        Ln.i("Device: " + Build.MANUFACTURER + " " + Build.MODEL + " (Android " + Build.VERSION.RELEASE + ")");
        final Device device = new Device(options);

        Thread initThread = startInitThread(options);

        int scid = options.getScid();
        boolean tunnelForward = options.isTunnelForward();
        boolean control = options.getControl();
        boolean audio = options.getAudio();
        boolean sendDummyByte = options.getSendDummyByte();

        Workarounds.prepareMainLooper();

        // Workarounds must be applied for Meizu phones:
        //  - <https://github.com/Genymobile/scrcpy/issues/240>
        //  - <https://github.com/Genymobile/scrcpy/issues/365>
        //  - <https://github.com/Genymobile/scrcpy/issues/2656>
        //
        // But only apply when strictly necessary, since workarounds can cause other issues:
        //  - <https://github.com/Genymobile/scrcpy/issues/940>
        //  - <https://github.com/Genymobile/scrcpy/issues/994>
        if (Build.BRAND.equalsIgnoreCase("meizu")) {
            Workarounds.fillAppInfo();
        }

        // Before Android 11, audio is not supported.
        // Since Android 12, we can properly set a context on the AudioRecord.
        // Only on Android 11 we must fill the application context for the AudioRecord to work.
        if (audio && Build.VERSION.SDK_INT == Build.VERSION_CODES.R) {
            Workarounds.fillAppContext();
        }

        List<AsyncProcessor> asyncProcessors = new ArrayList<>();

        try (DesktopConnection connection = DesktopConnection.open(scid, tunnelForward, audio, control, sendDummyByte)) {
            if (options.getSendDeviceMeta()) {
                connection.sendDeviceMeta(Device.getDeviceName());
            }

            if (control) {
                Controller controller = new Controller(device, connection, options.getClipboardAutosync(), options.getPowerOn());
                device.setClipboardListener(text -> controller.getSender().pushClipboardText(text));
                asyncProcessors.add(controller);
            }

            if (audio) {
                AudioCodec audioCodec = options.getAudioCodec();
                Streamer audioStreamer = new Streamer(connection.getAudioFd(), audioCodec, options.getSendCodecMeta(),
                        options.getSendFrameMeta());
                AsyncProcessor audioRecorder;
                if (audioCodec == AudioCodec.RAW) {
                    audioRecorder = new AudioRawRecorder(audioStreamer);
                } else {
                    audioRecorder = new AudioEncoder(audioStreamer, options.getAudioBitRate(), options.getAudioCodecOptions(),
                            options.getAudioEncoder());
                }
                asyncProcessors.add(audioRecorder);
            }

            Streamer videoStreamer = new Streamer(connection.getVideoFd(), options.getVideoCodec(), options.getSendCodecMeta(),
                    options.getSendFrameMeta());
            ScreenEncoder screenEncoder = new ScreenEncoder(device, videoStreamer, options.getVideoBitRate(), options.getMaxFps(),
                    options.getVideoCodecOptions(), options.getVideoEncoder(), options.getDownsizeOnError());

            for (AsyncProcessor asyncProcessor : asyncProcessors) {
                asyncProcessor.start();
            }

            try {
                // synchronous
                screenEncoder.streamScreen();
            } catch (IOException e) {
                // Broken pipe is expected on close, because the socket is closed by the client
                if (!IO.isBrokenPipe(e)) {
                    Ln.e("Video encoding error", e);
                }
            }
        } finally {
            Ln.d("Screen streaming stopped");
            initThread.interrupt();
            for (AsyncProcessor asyncProcessor : asyncProcessors) {
                asyncProcessor.stop();
            }

            try {
                initThread.join();
                for (AsyncProcessor asyncProcessor : asyncProcessors) {
                    asyncProcessor.join();
                }
            } catch (InterruptedException e) {
                // ignore
            }
        }
    }

    private static Thread startInitThread(final Options options) {
        Thread thread = new Thread(() -> initAndCleanUp(options));
        thread.start();
        return thread;
    }

    public static void main(String... args) throws Exception {
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            Ln.e("Exception on thread " + t, e);
        });

        Options options = Options.parse(args);

        Ln.initLogLevel(options.getLogLevel());

        if (options.getListEncoders() || options.getListDisplays()) {
            if (options.getCleanup()) {
                CleanUp.unlinkSelf();
            }

            if (options.getListEncoders()) {
                Ln.i(LogUtils.buildVideoEncoderListMessage());
                Ln.i(LogUtils.buildAudioEncoderListMessage());
            }
            if (options.getListDisplays()) {
                Ln.i(LogUtils.buildDisplayListMessage());
            }
            // Just print the requested data, do not mirror
            return;
        }

        try {
            scrcpy(options);
        } catch (ConfigurationException e) {
            // Do not print stack trace, a user-friendly error-message has already been logged
        }
    }
}
