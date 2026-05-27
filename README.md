# RootACS

## Русский

### Назначение

`RootACS` — пакет файлов для установки и использования службы `RootACS-Service` и консольного клиента `rootacs` в Windows.

### Содержимое каталога

- `release`
  Финальные исполняемые файлы `RootACS-Service.exe` и `rootacs.exe`.
- `source`
  Исходный код проекта.
- `setup.bat`
  Сценарий установки.
- `uninstall.bat`
  Сценарий удаления.
- `RootACS.iss`
  Конфигурационный файл Inno Setup.

### Установка

1. Установите и запустите `https://github.com/HZrate/RootACS/releases/download/rootacs-v1.0.0/RootACS-Setup-1.0.0-Windows10-x64-realese.exe`.
2. Подтвердите повышение прав администратора.
3. Дождитесь завершения установки.

Во время установки выполняются следующие действия:

- файлы копируются в `C:\Program Files\RootACS`;
- создаётся или обновляется служба `RootACS-Service`;
- каталог установки добавляется в системную переменную `PATH`;
- служба запускается.

### Использование

1. Убедитесь, что служба `RootACS-Service` запущена.
2. Запустите `rootacs.exe`.
3. Выберите целевую оболочку.
4. Используйте консольную сессию.

### Удаление

1. Запустите `uninstall.bat` с правами администратора.

Во время удаления выполняются следующие действия:

- завершается `rootacs.exe`, если процесс запущен;
- останавливается и удаляется служба `RootACS-Service`;
- каталог `C:\Program Files\RootACS` удаляется;
- путь установки удаляется из системной переменной `PATH`.

### Примечания

- В `Release` используется именованный канал `\\.\pipe\RootAccessPipe`.
- Доступ к `Release`-службе ограничен локальными администраторами и `SYSTEM`.
- `README.md`, `source` и `RootACS.iss` не требуются для работы установленной версии.

## English

### Purpose

`RootACS` is a package for installing and using the `RootACS-Service` service and the `rootacs` console client on Windows.

### Directory Contents

- `release`
  Final binaries: `RootACS-Service.exe` and `rootacs.exe`.
- `source`
  Project source code.
- `setup.bat`
  Installation script.
- `uninstall.bat`
  Removal script.
- `RootACS.iss`
  Inno Setup configuration file.

### Installation

1. Download and run `https://github.com/HZrate/RootACS/releases/download/rootacs-v1.0.0/RootACS-Setup-1.0.0-Windows10-x64-realese.exe`.
2. Approve the administrator elevation request.
3. Wait for the installation to complete.

The installation performs the following actions:

- copies files to `C:\Program Files\RootACS`;
- creates or updates the `RootACS-Service` service;
- adds the installation directory to the system `PATH`;
- starts the service.

### Usage

1. Ensure that the `RootACS-Service` service is running.
2. Run `rootacs.exe`.
3. Select the target shell.
4. Use the console session.

### Removal

1. Run `uninstall.bat` with administrator rights.

The removal performs the following actions:

- terminates `rootacs.exe` if it is running;
- stops and deletes the `RootACS-Service` service;
- removes `C:\Program Files\RootACS`;
- removes the installation directory from the system `PATH`.

### Notes

- `Release` uses the named pipe `\\.\pipe\RootAccessPipe`.
- Access to the `Release` service is limited to local administrators and `SYSTEM`.
- `README.md`, `source`, and `RootACS.iss` are not required for the installed version.
