# 제삼자 고지

HMJE의 제한 라이선스는 아래 제삼자 자료의 원래 권리를 변경하지 않습니다.
폰트를 앱에 포함하면 각 폰트의 라이선스 파일도 함께 배포하십시오.

| 포함 자료 | 위치 | 원래 라이선스 |
| --- | --- | --- |
| SQLite 3.53.4, 커밋 `bf7c7f30031888f4e796e429ab3978879485` | `library/Native/SQLite/vendor/sqlite3.c`, `sqlite3.h` | 퍼블릭 도메인 |
| D2Coding 1.3.2 | `libraries/hmje-ui/assets/fonts/D2Coding-*` | SIL Open Font License 1.1 |
| Montserrat 9.000 | `libraries/hmje-ui/assets/fonts/Montserrat-*` | SIL Open Font License 1.1 |
| Noto Color Emoji 2.051 | `libraries/hmje-ui/assets/fonts/NotoColorEmoji*` | SIL Open Font License 1.1 |
| Pretendard 1.309 | `libraries/hmje-ui/assets/fonts/Pretendard-*` | SIL Open Font License 1.1 |
| Unicode 17.0 줄바꿈·문자소 분리·이모지 속성 데이터 | `libraries/hmje-ui/assets/unicode/unicode-17.0-break.bin`, `libraries/hmje-ui/Sources/HmjeGraphics/graphics/emoji_properties.hmje` | [Unicode License v3](UNICODE-LICENSE.txt) |
| SUITE 2.040 | `libraries/hmje-ui/assets/fonts/SUITE-*` | SIL Open Font License 1.1 |

SQLite 저자는 코드를 퍼블릭 도메인으로 제공하며 상용·비상용 복제·수정·배포를
허용합니다. 원본 파일의 고지는 보존했습니다. [SQLite 공식 고지](https://www.sqlite.org/copyright.html).

폰트의 저작권자·예약 이름·OFL 전문은 같은 폴더의 `*-LICENSE.txt`에 있습니다.
라이선스 원문을 축약하거나 HMJE 라이선스로 대체하지 않았습니다.
Pretendard의 비트맵 파생 폰트 `pretendard-regular-28x40-ko-alpha.bfnt`도
동일한 OFL을 따릅니다. Unicode 데이터와 파생 표를 앱에 포함할 때에는
[Unicode License v3 전문](UNICODE-LICENSE.txt)을 함께 보존하십시오.

LLVM/clang은 외부 빌드 의존성이고 이 소스 트리에 벤더링하지 않습니다.
LLVM을 별도 배포할 때에는 [LLVM의 Apache 2.0 및 LLVM 예외](https://llvm.org/LICENSE.txt)를
따라야 합니다. 운영체제 SDK·시스템 라이브러리·선택적으로 연동하는 외부 엔진도
이 트리에 포함되지 않으며 각 제공자의 조건을 따릅니다.
