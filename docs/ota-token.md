# Creating or changing the OTA token

The Enviro8 accepts a firmware update over the network (`POST /api/v1/firmware`, or the upload box on `/debug`) only when the request carries the right token in the `X-OTA-Token` header. Anyone who can reach the device and knows the token can install firmware on it, so treat the token like a password.

## How the token works

- **It is compiled into the firmware.** The device has no setting for it and no API to change it. A new token takes effect only when you install a build that contains it.
- **It lives in `sdkconfig.defaults.local`** at the repo root, as `CONFIG_ENVIRO8_OTA_TOKEN="..."`. That file is git-ignored and never goes to GitHub.
- **The build copies it into `sdkconfig`**, also git-ignored. The build reads the token from `sdkconfig`, so editing only `sdkconfig.defaults.local` is not enough (step 3).
- **Limit:** up to 128 characters. An empty token turns network updates off: the device answers `403`.

## Creating a token (first time)

Use this when `sdkconfig.defaults.local` does not exist yet, for example on a fresh clone of the repo. Without it, the firmware is built with an empty token and network updates are turned off.

Run these in the **ESP-IDF v5.5.5** Windows Terminal profile, from the repo root (`cd $env:USERPROFILE\git\enviro8`).

1. **Create the token file** with a random 48-character token. The command also prints the token:

   ```powershell
   $b = New-Object byte[] 24; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b); $new = -join ($b | ForEach-Object { $_.ToString('x2') }); Set-Content -Encoding ascii sdkconfig.defaults.local "CONFIG_ENVIRO8_OTA_TOKEN=`"$new`""; $new
   ```

   Store it in a password manager. To choose your own token instead, create the file by hand with one line, `CONFIG_ENVIRO8_OTA_TOKEN="your-token"`, up to 128 characters.

2. **Delete `sdkconfig`** if a build was already done without the token: `Remove-Item sdkconfig`. On a fresh clone there is none.

3. **Build:** `idf.py set-target esp32c6`, then `idf.py build`.

4. **Install the build:**
   - **Over the network,** if the device already runs firmware with a token: upload it as in step 5 below, using that existing token.
   - **Over USB,** if the device runs firmware without a token (it answers `403`) or has never been flashed: follow [Updating over USB](#updating-over-usb-no-token-needed).

5. **Check it** as in step 6 below: the new token must get `400 "not an Enviro8 firmware image"`.

## Changing an existing token

Run these in the same ESP-IDF v5.5.5 profile, from the repo root.

### 1. Note the current token

```powershell
Get-Content sdkconfig.defaults.local
```

Keep that value. The device still checks the **old** token until the new firmware is running, so you need it for the upload in step 5.

### 2. Generate a new token and save it

```powershell
$b = New-Object byte[] 24; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b); $new = -join ($b | ForEach-Object { $_.ToString('x2') }); Set-Content -Encoding ascii sdkconfig.defaults.local "CONFIG_ENVIRO8_OTA_TOKEN=`"$new`""; $new
```

This writes a random 48-character token to `sdkconfig.defaults.local` and prints it. Store it in a password manager. If it is lost, the only way to update the device is over USB.

### 3. Make the build pick it up

Delete the generated `sdkconfig` so it is rebuilt from the defaults files:

```powershell
Remove-Item sdkconfig
```

The alternative is `idf.py menuconfig` → **Enviro8** → **Firmware upload token**.

### 4. Build

```powershell
idf.py set-target esp32c6
idf.py build
```

Check that the token went in, without printing it:

```powershell
(Select-String -Path sdkconfig -Pattern '^CONFIG_ENVIRO8_OTA_TOKEN="(.+)"$').Matches[0].Groups[1].Value -eq $new
```

It must print `True`.

### 5. Upload the new build with the OLD token

```powershell
curl.exe -s -X POST -H "X-OTA-Token: <old token>" --data-binary "@build/enviro8.bin" http://10.0.37.130/api/v1/firmware
```

Expect `{"ok":true,"rebooting":true}`. The device writes the inactive slot and reboots into it, which takes about 5 seconds.

You can also open `http://10.0.37.130/debug`, choose `build/enviro8.bin`, enter the **old** token and click **Téléverser**.

### 6. Check that the new token is active

These checks send the bootloader file. The device checks the token first, then refuses the file because it is not Enviro8 firmware, so nothing is installed and the device does not reboot.

```powershell
curl.exe -s -w " -> %{http_code}`n" -X POST -H "X-OTA-Token: $new" --data-binary "@build/bootloader/bootloader.bin" http://10.0.37.130/api/v1/firmware
curl.exe -s -w " -> %{http_code}`n" -X POST -H "X-OTA-Token: <old token>" --data-binary "@build/bootloader/bootloader.bin" http://10.0.37.130/api/v1/firmware
```

| Token | Expected answer | Meaning |
|---|---|---|
| new | `not an Enviro8 firmware image -> 400` | New token accepted |
| old | `missing or wrong X-OTA-Token -> 401` | Old token no longer works |

`GET http://10.0.37.130/api/v1/status` should also show `"firmwarePendingVerify":false`.

## If something goes wrong

| Problem | Cause and fix |
|---|---|
| Upload answers `401` in step 5 | Wrong old token. The device still runs the old firmware, so nothing changed. |
| Upload answers `403` | The running firmware was built with an empty token. Update it over USB (below). |
| New token gets `401` after the update | The build still had the old token: `sdkconfig` was not deleted (step 3). Rebuild and upload again with the old token. |
| Token lost | Update over USB (below). Settings are kept. |

### Updating over USB (no token needed)

Plug the Enviro8 into the PC by USB, then:

```powershell
idf.py -p COM20 flash
```

The COM port may differ: it is the "USB Serial Device" with VID `303A` in Device Manager. Use `flash`, never `erase-flash`. `flash` keeps the Wi-Fi, IP and channel settings; `erase-flash` wipes them.

To be safe, take a settings backup first:

```powershell
python tools/enviro8_backup.py backup --port COM20
```
