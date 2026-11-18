# Сторонние компоненты

Graph3D использует следующие внешние библиотеки:

| Компонент | Назначение | Сведения от разработчика |
|---|---|---|
| Qt 6 (Qt Widgets) | Интерфейс, окна редакторов и рисование чертежей | [Лицензирование Qt](https://doc.qt.io/qt-6/licensing.html) |
| Open CASCADE Technology (OCCT) | Геометрия, обмен моделями и 3D-сцена | [Лицензия OCCT и исключение](https://github.com/Open-Cascade-SAS/OCCT/blob/V7_9_3/OCCT_LGPL_EXCEPTION.txt) |
| FFmpeg | Динамическая зависимость библиотеки OCCT TKService | [Лицензирование FFmpeg](https://ffmpeg.org/legal.html) |

Комплект для Windows включает динамические библиотеки. Точные версии, файлы
и ссылки на исходники перечислены в `THIRD_PARTY_RUNTIME.md` в установленной
папке. Уведомления и тексты лицензий находятся в `licenses`. Библиотеки можно
заменить совместимыми сборками. Для FFmpeg поставляется отдельная минимальная
сборка без GPL-компонентов; её параметры описаны в
[`packaging/windows/build-ffmpeg.sh`](packaging/windows/build-ffmpeg.sh).

Обмен DWG использует отдельно установленные программы `dwg2dxf` и `dxf2dwg`
из GNU LibreDWG. Они запускаются отдельными процессами. GNU LibreDWG
распространяется под GNU GPL версии 3 или новее. [GNU LibreDWG](https://www.gnu.org/software/libredwg/)
