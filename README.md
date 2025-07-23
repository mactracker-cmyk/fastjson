# FastJSON Python C API ⚡

![Python](https://img.shields.io/badge/Python-3.12+-blue?logo=python)
![License](https://img.shields.io/badge/License-MIT-green)
![Build](https://img.shields.io/badge/Build-CMake-success)

Высокопроизводительная библиотека для работы с JSON, реализованная на C с Python C API.

## 📦 Особенности
- В 2 раза быстрее стандартного `json` модуля Python
- Низкоуровневая реализация на C
- Поддержка всех основных операций с JSON
- Минимальные накладные расходы
- Совместимость с Python 3.12+

## 🛠 Установка

Библиотека требует компиляции. Убедитесь, что у вас установлены:

- Python 3.12+ (включая development headers)
- CMake 3.12+
- C компилятор (GCC, MinGW, MSVC)

### Шаг 1: Подготовка окружения

#### Для Linux (Debian/Ubuntu):
```bash
sudo apt-get update
sudo apt-get install -y build-essential python3-dev cmake
