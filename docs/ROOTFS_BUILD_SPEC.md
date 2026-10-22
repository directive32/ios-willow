# Повна специфікація підготовки та збірки iOS 16 RootFS (HFS+)

Цей документ фіксує перевірені та успішно відпрацьовані параметри формування образу кореневої файлової системи iOS 16.7.16 (`20H392`) для Xiaomi Redmi Note 8T (`willow` / Snapdragon 665).

---

## 1. Цільові образи та джерело (IPSW)

* **Модель:** iPhone X (`iPhone10,3` / `iPhone10,6`, плата `D22AP`)
* **Версія ОС:** iOS 16.7.16 (Build `20H392`)
* **Офіційне посилання Apple CDN:**  
  `https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/1158E502-D76D-4226-AA06-ED5E30CD1111/iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw`
* **Необхідні DMG-диски з IPSW:**
  1. `087-86698-044.dmg` (~4.5 GB) — Основна коренева система (OS root, UDIF DMG з APFS)
  2. `087-86593-047.dmg` (~2.6 GB) — Системний криптекс (SystemOS: dyld shared cache, системні бібліотеки)
  3. `087-86715-047.dmg` (~10 MB) — Додатковий криптекс додатків (AppOS)

---

## 2. Середовище виконання (Host OS & Toolchain)

* **Базова ОС:** `Ubuntu 22.04 LTS` (LTS середовище з перевіреним GCC 11 та CMake 3.22).
* **Критичний драйвер ядра:**
  * У стандартних серверних/хмарних ядрах модуль `hfsplus.ko` винесено в `linux-modules-extra`.
  * **Команда встановлення та завантаження:**
    ```bash
    sudo apt-get update
    sudo apt-get install --no-install-recommends -y linux-modules-extra-$(uname -r)
    sudo modprobe hfsplus
    ```
* **Пакетні залежності:**
  ```bash
  sudo apt-get install --no-install-recommends -y \
    build-essential cmake git libfuse3-dev zlib1g-dev \
    bzip2 libbz2-dev libattr1-dev hfsprogs android-sdk-libsparse-utils zstd rsync curl unzip
  ```

---

## 3. Збірка `apfs-fuse` (драйвер читання APFS)

* **Репозиторій:** `https://github.com/sgan81/apfs-fuse.git` (з усіма субмодулями `3rdparty/lzfse`)
* **Критичний патч:** У файлі `ApfsLib/PList.h` обов'язково має бути підключено стандартний заголовок цілих типів `<stdint.h>` (НЕ `<cstdint>`, щоб не ламати компіляцію C-коду в `lzfse`):
  ```bash
  git clone --recurse-submodules https://github.com/sgan81/apfs-fuse.git
  cd apfs-fuse
  sed -i '1i #include <stdint.h>' ApfsLib/PList.h
  mkdir build && cd build
  cmake ..
  make -j$(nproc)
  sudo cp apfs-fuse /usr/local/bin/
  ```

---

## 4. Скрипт збірки образу (`tools/mkiosroot.sh`)

* **Команда запуску:**
  ```bash
  bash tools/mkiosroot.sh 087-86698-044.dmg 087-86593-047.dmg 087-86715-047.dmg ios16-root.img 16G
  ```
* **Внутрішні кроки та параметри:**
  1. **Монтування через FUSE:**  
     Кожен DMG монтується окремо через `apfs-fuse <file.dmg> <mountpoint>`, що розпаковує блоки LZBITMAP прозоро для rsync.
  2. **Створення HFS+ розділу:**  
     `truncate -s 16G ios16-root.img`  
     `mkfs.hfsplus -s -J -v iOSRoot ios16-root.img` (Case-sensitive, з журналюванням 8192k).
  3. **Монтування цільового диска в Linux:**  
     `sudo mount -t hfsplus -o loop,force ios16-root.img /work/root`
  4. **Копіювання зі збереженням прав і жорстких посилань:**  
     `sudo rsync -aH --numeric-ids <source>/ <dest>/`  
     - OS root -> `/`
     - SystemOS -> `/private/preboot/Cryptexes/OS`
     - AppOS -> `/private/preboot/Cryptexes/App`
  5. **Лінкування кешу dyld:**  
     Жорсткі посилання з `private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/*` у `/System/Library/Caches/com.apple.dyld/` (щоб dyld на рівні запалювання 1 бачив спільні бібліотеки без окремого драйвера cryptex).
  6. **Лінкування відсутніх файлів cryptex:**  
     Усі бібліотеки cryptex, яких немає в корені, лінкуються в корінь (`ln` або `cp -P` для сімлінків).
  7. **Створення системних структур:**
     - `/private/var/db/diagnostics` та `/private/var/db/uuidtext` (UID/GID 272 `_logd` з маркерами `.migrated`).
     - `/private/var/keybags/systembag.kb` (базовий відкритий системний кейбег для AppleKeyStore без перевірки тач-айді/сепі).
  8. **Перевірка fsck:**  
     `sudo umount` + `fsck.hfsplus -f -y ios16-root.img` для узгодження таблиць імен файлів.

---

## 5. Твіки першого запуску (`tools/sweet-root-tweaks.py`)

* **Монтування:**
  ```bash
  sudo mkdir -p /mnt/iosroot
  sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
  sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
  sudo umount /mnt/iosroot
  sudo fsck.hfsplus -fy ios16-root.img || true
  ```
* **Що змінює скрипт:**
  - `launchd.plist`: `_PanicOnCrash` замінено на `_PanicOnCrasX` (падіння одного демона не відправляє ядро в паніку); видалено сервіси `CommCenter` (відсутність модема більше не блокує завантаження).
  - `SpringBoard`: `BKNoWatchdogs: true` (вимкнено сторожовий таймер, який вбиває повільні процеси під час старту на 1 ядрі).
  - `Setup (PurpleBuddy)`: прапорці `SetupDone: true`, `SetupFinishedAllSteps: true` (пропуск вітального екрана "Hello").
  - `Audio / MediaToolbox`: вимкнено `VirtualAudio.plugin` та `StartRecorderServer=false` (запобігає краш-лупам mediaserverd).
  - `Контейнери`: створення директорій `/private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}`.

---

## 6. Конвертація в Android Sparse Image (Fastboot)

* **Команди:**
  ```bash
  img2simg ios16-root.img ios16-root.simg
  zstd -T0 -3 ios16-root.simg -o ios16-root.simg.zst
  ```
* **Результат:** Sparse-образ `ios16-root.simg`, готовий до безпечного прошивання через Fastboot у розділ `userdata`:
  ```bash
  fastboot -S 512M flash userdata ios16-root.simg
  ```
