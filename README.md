# HMJE — 훈민정음

HMJE는 **한국어 문법을 쓰는 정적 타입 컴파일 언어**입니다. 프런트엔드,
타입 검사기와 HMIR 생성기는 HMJE로 작성되어 자체 호스팅합니다. 코드 생성
백엔드는 **LLVM**이며, **clang을 드라이버로 사용**해 운영체제별 네이티브
실행 파일을 만듭니다.

**1.0.0-beta.1**은 첫 공개 Beta입니다. 문법·API·배포 계약은 안정화 중이며
다음 Beta에서 호환성이 바뀔 수 있습니다. [알려진 제한과 릴리스 노트](RELEASE_NOTES-1.0.0-beta.1.md)를
확인하십시오. 훈민정음 IDE도 HMJE로 작성했지만 **IDE 본체 소스는 비공개**이며
바이너리는 [Beta 1 릴리스](https://github.com/Semtle-Inc/hmje/releases/tag/v1.0.0-beta.1)에서 제공합니다.

## 버전 및 소스

이 저장소는 **HMJE 1.0.0-beta.1 · 빌드 `2026100804`**의 컴파일러·런타임·SDK 소스를 제공합니다. 릴리스 날짜는 **2026-10-09**입니다.

구성요소별 원본 버전과 배포 소스의 차이는 [소스 출처](SOURCES.md), 확인한 환경과 결과는 [검증 결과](docs/publication-status.md)에서 확인할 수 있습니다.

## 설치

운영체제와 아키텍처에 맞는 패키지를 내려받습니다. 패키지에는 **훈민정음 IDE와 툴체인**이 포함됩니다. 동봉된 설치 안내에 따라 설치한 뒤 `hmjec --version`으로 명령줄 툴체인을 확인합니다.

| 호스트 | ARM64 | x86_64 / x64 |
| --- | --- | --- |
| macOS 14 이상 | [DMG](https://github.com/Semtle-Inc/hmje/releases/download/v1.0.0-beta.1/Hunmin-1.0.0-beta.1-2026100804-macos-arm64.dmg) | [DMG](https://github.com/Semtle-Inc/hmje/releases/download/v1.0.0-beta.1/Hunmin-1.0.0-beta.1-2026100804-macos-x86_64.dmg) |
| Windows 11 | [ZIP](https://github.com/Semtle-Inc/hmje/releases/download/v1.0.0-beta.1/Hunmin-1.0.0-beta.1-2026100804-windows-arm64.zip) | [ZIP](https://github.com/Semtle-Inc/hmje/releases/download/v1.0.0-beta.1/Hunmin-1.0.0-beta.1-2026100804-windows-x64.zip) |
| Linux | [tar.gz](https://github.com/Semtle-Inc/hmje/releases/download/v1.0.0-beta.1/Hunmin-1.0.0-beta.1-2026100804-linux-arm64.tar.gz) | [tar.gz](https://github.com/Semtle-Inc/hmje/releases/download/v1.0.0-beta.1/Hunmin-1.0.0-beta.1-2026100804-linux-x86_64.tar.gz) |

파일 무결성 확인에는 릴리스의 [SHA256SUMS](https://github.com/Semtle-Inc/hmje/releases/download/v1.0.0-beta.1/SHA256SUMS)를 사용합니다.

HMJE 앱을 빌드하는 데 필요한 운영체제 개발 도구와 SDK는 별도로 설치합니다.

| 호스트 | 필요한 것 |
| --- | --- |
| macOS | Xcode Command Line Tools(`xcode-select --install`). iOS 앱·`.xcassets` 아이콘·Metal(hmje-sem, hmje-llm)은 Xcode와 iOS 시뮬레이터 런타임 |
| Windows | LLVM clang(`winget install LLVM.LLVM`), MSVC Build Tools와 Windows SDK |
| Linux | glibc 2.38 이상(Ubuntu 24.04·Debian 13·Fedora 39 이상), clang, OpenSSL 3 개발 헤더, libX11 개발 헤더, X11 또는 XWayland |
| Android | Android SDK·NDK·JDK(Android Studio). Linux arm64 호스트는 지원하지 않습니다 |

설치 명령과 증상별 해결 방법은 [릴리스 노트의 설치 전 준비물](RELEASE_NOTES-1.0.0-beta.1.md#설치-전-준비물)에
있습니다. 지원 표는 모든 플랫폼 API의 동작이 같다는 뜻은 아닙니다.

## 첫 프로그램

`examples/hello.hmje`:

```hmje
함수 앱시작은
    "HMJE가 실행되었습니다."를 출력한다.
```

macOS·Linux:

```sh
hmjec --target posix-host-exe examples/hello.hmje -o hello
./hello
```

Windows PowerShell:

```powershell
hmjec.exe --target windows-exe examples/hello.hmje -o hello.exe
.\hello.exe
```

## 예제

아래 예제의 전체 소스는 `examples/`에 있습니다. Beta 1 소스에서 빌드한 툴체인으로 컴파일을 확인했습니다. 화면 예제의 검증 범위는 앱 빌드입니다.

### 타입과 이름 붙인 호출

기본 숫자 타입은 `자연수`, `정수`, `실수`입니다. 실행문은 함수와 인자를
문장으로 적고, 문장형 조건 비교는 `A가 B이면`으로 씁니다.

```hmje
함수 인사는 (이름: 문자열, 횟수: 자연수)를 받아
    이름을 출력한다.
    횟수를 출력한다.

함수 앱시작은
    상수 개수는 3이고, 자연수이다.
    상수 온도는 -2이고, 정수이다.
    상수 비율은 0.5이고, 실수이다.
    인사를 (이름: "HMJE", 횟수: 개수)로 실행한다.
    만약 개수가 3이면
        온도를 출력한다.
        비율을 출력한다.
```

### 종류와 패턴 매칭

```hmje
도형의 종류는
    원(반지름: 실수)과,
    점이다.

함수 설명은 (모양: 도형)를 받아
    모양이 원(반지름)인 경우
        반지름을 출력한다.
    점인 경우
        "점"을 출력한다.

함수 앱시작은
    설명을 (모양: 도형.원(반지름: 2.0))으로 실행한다.
```

### 타입 있는 실패

```hmje
입력오류의 종류는
    빈이름이다.

입력오류로 실패할 수 있는 함수 확인은 (이름: 문자열)를 받아
    만약 이름이 ""이면
        입력오류.빈이름으로 실패한다.
    이름을 출력한다.

함수 앱시작은
    시도할 작업은
        확인을 (이름: "")으로 실행한다.
    실패하면
        실패값이 빈이름인 경우
            "이름을 입력하세요"를 출력한다.
```

### 비동기 작업

```hmje
비동기인 함수 준비는 (이름: 문자열)를 받아
    1밀리초 기다린다.
    이름을 출력한다.

비동기인 함수 앱시작은
    준비를 (이름: "준비 완료")로 실행한다.
```

반환값이 있는 비동기 호출에는 `기다린 결과`를 사용합니다.
구조화한 동시 작업·취소·시간 제한은 [언어 문서](docs/language.md)에 있습니다.

### 화면

공개 `hmje-ui`의 기존 텍스트 컴포넌트를 사용하는 최소 화면입니다.

```hmje
HmjeUI를 가져온다.

화면 인사화면의
    그리는 내용은
        텍스트의
            내용은 "HMJE가 실행되었습니다."이다.

화면을 표시하는 함수 앱시작은
    그리는 내용은
        인사화면이다.
```

macOS에서는 `hmjec --target macos-app examples/screen.hmje -o Hello.app`으로
앱을 만들 수 있습니다. Linux·Windows는 해당 호스트 실행 파일 대상을 사용합니다.

## 이 저장소

| 경로 | 내용 |
| --- | --- |
| `compiler/` | HMJE 프런트엔드·타입 검사·HMIR·LLVM 생성과 컴파일러 전용 호스트 연결 |
| `library/` | 표준·기반·운영체제 API·DB 공통 모형·SQLite·Backend |
| `platforms/` | 운영체제·플랫폼 경계의 네이티브 연결 코드 |
| `libraries/` | [공개 라이브러리 안내](libraries/README.md)와 고정 소스 |
| `docs/` | [언어](docs/language.md), [형식 문법](docs/grammar.md), [소스 빌드](docs/building.md) |
| `examples/`, `tests/` | HMJE 예제와 Python 없이 실행하는 스모크 검사 |
| `tools/`, `bin/` | 빌드·설치에 필요한 최소 도구 |

| 공개 라이브러리 | 용도 |
| --- | --- |
| hmje-llm | GGUF 언어 모델·토크나이저·로컬 추론 |
| hmje-sem | 텐서·자동 미분·CPU/Metal 연산 |
| hmje-realtime | 실시간 통신 |
| hmje-network | 네트워크·HTTP |
| hmje-ui | 화면·그래픽·데스크톱 UI |
| hmje-crypto | 암호 API |
| hmje-vault | 자격 증명 저장 |
| hmje-jindo | 터미널 진행 막대 |
| hmje-host | 파일·프로세스 호스트 API |
| hmje-syntax | HMJE 구문 강조 |

IDE 본체, 비공개 디자인 시스템·내부 라이브러리, 오디오 라이브러리 및
내부 데이터베이스 엔진·PostgreSQL 어댑터는 공개 범위에 포함하지 않습니다. SQLite는 포함합니다.
정확한 출처 커밋은 [SOURCES.md](SOURCES.md)에 있습니다.

GitHub Linguist의 HMJE 자동 분류는 지원되지 않습니다. `.gitattributes`에는
벤더 코드와 운영체제 연결 코드의 분류를 지정합니다.

## 소스 빌드와 검사

Release 툴체인을 초기 컴파일러로 사용하거나, [hmje-swift-seed](https://github.com/Semtle-Inc/hmje-swift-seed)의
Swift 씨앗에서 구축한 컴파일러로 현재 소스를 빌드할 수 있습니다.
[빌드 안내](docs/building.md)와 [Swift 부트스트랩](docs/bootstrap.md)에
요구 환경·실행 명령·검증 범위를 설명합니다. 공개 라이브러리는 이 트리에 포함되어
소스 빌드 중 별도 저장소에서 내려받지 않습니다.

```sh
sh tools/build-from-release.sh "$RELEASE_PREFIX/lib/hmje/libexec/bin" "$PWD/build/native-bin"
HMJE_INSTALL_WARM=0 sh tools/install-native.sh "$PWD/build/install" "$PWD/build/native-bin"
HMJEC="$PWD/build/install/bin/hmjec" sh tools/test.sh
```

## 관련 저장소

- [개발자 세션 예제](https://github.com/Semtle-Inc/hmje-developer-sessions): 문법·UI·서버·모델 학습·IDE 예제
- [Swift 씨앗 재현 검증](https://github.com/Semtle-Inc/hmje-swift-seed): Swift 초기 컴파일러에서 Beta 1 도구를 구축하고 배포 파일과 비교하는 절차

## 라이선스와 상표

Beta 1 라이선스 버전 1.0(2026-10-09)을 적용합니다. 소스 공개 라이선스이며
OSI 오픈 소스 라이선스가 아닙니다. [LICENSE](LICENSE)는 상용 앱 개발을
포함한 무료 사용을 허용하지만 도구의 판매·재라이선스·재배포와 경쟁 제품·
서비스 제공을 제한합니다. [LICENSE-RUNTIME](LICENSE-RUNTIME)은 런타임·
라이브러리를 상용 앱 안에 포함해 배포하는 것을 허용하며, 정보 화면이나
문서에 **“훈민정음(HMJE)으로 만들었습니다” / “Built with HMJE”** 중 하나를
표시해야 합니다. [제삼자 고지](THIRD_PARTY_NOTICES.md)도 확인하십시오.
IDE 패키지에 들어 있는 아이콘 글꼴·디자인 자료·이미지는 IDE 전용 자산으로, IDE 밖에서
재사용할 수 없습니다([IDE 포함 자산 고지](IDE-ASSETS-NOTICE.md)).

“HMJE”와 “훈민정음 IDE”는 저작권자 오현우의 이름·상표이며 상표 권리를 부여하지
않습니다. HMJE의 저작권은 오현우에게 있으며, Semtle Inc.가 공개 배포합니다. LLVM·SQLite와 포함된
폰트의 저자에게 감사드립니다. 문제를 발견하면 이 저장소의 **Issues**에
운영체제·아키텍처·`hmjec --version` 출력과 최소 재현 소스를 첨부해 주십시오.
