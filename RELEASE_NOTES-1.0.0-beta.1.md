# HMJE 1.0.0-beta.1

2026-10-09. 첫 공개 Beta입니다. 문법·라이브러리·배포 계약은 아직 안정화 중이며
향후 Beta에서 호환성이 바뀔 수 있습니다. 문제 보고에는 이 저장소의 Issues를 이용해 주십시오.

## 제공 구성

한국어 문법의 정적 타입 컴파일 언어, HMJE로 작성한 자체 호스팅 컴파일러,
LLVM 기반 네이티브 코드 생성과 표준·공개 플랫폼 라이브러리 10개입니다.
훈민정음 IDE는 HMJE로 작성되어 있으며 [Beta 1 릴리스](https://github.com/Semtle-Inc/hmje/releases/tag/v1.0.0-beta.1)에서 바이너리를 제공합니다.
IDE 본체 소스는 이 저장소의 공개 범위에 포함하지 않습니다.

호스트 대상은 macOS 14+ arm64/x86_64, Windows 11 ARM64/x64,
Linux x86_64/arm64입니다. 호스트별 API·그래픽·패키징의 제약은 남아 있습니다.

## 언어와 컴파일러

- 기본 숫자 표기는 `자연수`, `정수`, `실수`입니다. 범위 밖 리터럴은 오류이고,
  명시 변환·산술·0 나누기·첨자·없는 선택값 해제는 실행 중 검사합니다.
  종류를 바꾸거나 축소하는 암시 변환의 일부는 이번 Beta에서 경고이며 다음
  릴리스에서 오류로 강화할 예정입니다. 자유 실행 커널의 기본 검사 정책은 별도입니다.
- `그리고`·`또는`는 단락 평가합니다. 상수 재대입, 중복 정의, 반환 누락,
  값으로 순환하는 구조체, 클로저 캡처 재대입을 컴파일 단계에서 진단합니다.
- 문장형 종류 선언과 페이로드 패턴 매칭, 개발자가 정의한 타입 있는 실패를
  동기·비동기·함수값에 사용할 수 있습니다.
- 진단은 소스 파일과 줄을 표시합니다. 파서·AST·타입 처리의 구조화와
  형식 문법 문서를 포함합니다. LLVM은 코드 생성 백엔드이고 clang은 연결 드라이버입니다.

## 런타임과 개발 도구

링크 작업 수와 공유 캐시를 개선하고, 클로저 참조 수와 비동기 취소·시간 제한의
경쟁 조건을 보완했습니다. Windows·Linux의 호스트 처리도 보완했습니다.
UI의 의도적인 해시 넘침과 음수 좌표를 숫자 안전 검사에 맞게 정리했고,
리터럴 배열 문맥 추론과 객체 동일성 검사 문제를 수정했습니다.

## 공개 범위와 제한

- `hmje-ui`는 공개 UI 라이브러리입니다. 비공개 디자인 시스템·내부 라이브러리와
  오디오 라이브러리는 공개 범위에 포함하지 않습니다.
- SQLite 3.53.4를 포함합니다. 내부 데이터베이스 엔진·PostgreSQL 어댑터는 제외합니다.
- 타입 있는 실패의 처리 블록 하나에서는 하나의 오류 종류를 다룹니다.
- 비교는 문장형 `A가 B이면`을 권장합니다. `==`와 문장형 비교의 허용 타입에
  차이가 남아 있으며 다음 Beta의 호환성 검토 대상입니다.
- `위험` 경계 진단 강화 등 일부 안전성 정책은 이후 릴리스에서 단계적으로
  적용합니다. 커널·모바일 배포는 이번 데스크톱 호스트 지원 표와 별도입니다.
- Windows와 Linux 지원은 macOS보다 검증 범위가 좁습니다. 이번 Beta에서 Windows·Linux의 설치·빌드·실행은 불안정할 수 있으며, 문제를 발견하면 Issues로 보고해 주십시오.
- Linux 패키지는 glibc 2.38 이상이 필요합니다(예: Ubuntu 24.04, Debian 13, Fedora 39 이상). 더 오래된 배포판에서는 `GLIBC_2.38 not found`로 실행되지 않습니다.
- Windows 네이티브 실행·Linux GUI의 전체 조합, 작은 메모리 환경, 모든 UI
  동작을 이 저장소 검증만으로 보증하지 않습니다. 플랫폼별 Release 자산과
  동봉 문서를 확인하십시오. 빌드 성공은 UI 실행 검증과 다릅니다.
- Linux에서 OpenSSL 개발 헤더(`openssl/x509.h`)가 없으면 `HmjeNetwork` 빌드가 실패합니다. IDE는 원인을 밝히지 않고 "네이티브 병렬 컴파일 실패"만 표시합니다. Ubuntu에서는 `libssl-dev`를 설치하십시오.
- Windows에서 압축 파일 안의 경로가 길어 Windows 경로 길이 제한(260자)에 걸리면 압축이 풀리지 않습니다. 받은 파일 이름을 `hunmin.zip`처럼 짧게 바꾸고, `C:\hunmin`처럼 짧은 위치에 압축을 푸십시오.
- Windows에서 빌드하면 `hmjec: LLVM clang.exe is required on Windows`로 실패할 수 있습니다. Windows 패키지에는 clang이 들어 있지 않고, `hmjec`는 PATH의 `clang.exe`나 환경 변수 `HMJE_WINDOWS_CLANG`만 찾습니다. 해결: (1) `winget install LLVM.LLVM`으로 LLVM을 설치합니다. (2) `C:\Program Files\LLVM\bin`을 PATH에 추가하거나, `HMJE_WINDOWS_CLANG`에 `clang.exe`의 전체 경로를 지정합니다. (3) IDE를 완전히 종료한 뒤 다시 실행합니다. 링크 단계에는 Visual Studio Build Tools의 'C++를 사용한 데스크톱 개발'(MSVC·Windows SDK)도 필요합니다.
- Beta 1 라이선스 버전 1.0을 적용합니다. 도구는 무료 사용이 가능하지만 재배포·
  경쟁 제공은 제한합니다. 앱에 포함한 런타임·라이브러리는 기여 표시를 해야 합니다.

구성요소별 소스 버전은 [소스 출처](SOURCES.md), 검증한 환경과 결과는 [배포 구성 및 검증 결과](docs/publication-status.md)에서 확인할 수 있습니다.

## 설치 전 준비물

패키지에는 훈민정음 IDE, 컴파일러와 라이브러리가 포함됩니다. 운영체제의 C 컴파일러와 SDK는 따로 설치해야 합니다.

### macOS (14 이상)
- **Xcode Command Line Tools (필수):** macOS 앱과 명령줄 프로그램에 쓰는 clang, macOS SDK, 코드 서명 도구. `xcode-select --install`
- **Xcode:** iOS 앱, `.xcassets` 앱 아이콘, Metal을 쓰는 hmje-sem·hmje-llm에 필요합니다. 설치한 뒤 Xcode를 기본 개발 도구로 지정하십시오. Command Line Tools가 지정되어 있으면 iOS SDK를 찾지 못합니다. `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`
- **iOS 시뮬레이터:** IDE에서 iOS로 실행하려면 iOS 시뮬레이터 런타임이 필요합니다. Xcode 설정의 Components에서 iOS를 받거나 `xcodebuild -downloadPlatform iOS`
- **Metal 도구:** hmje-sem·hmje-llm 빌드에서 Metal 컴파일러가 없다고 나오면 `xcodebuild -downloadComponent MetalToolchain`
- **iOS 기기:** IDE의 iOS 실행은 시뮬레이터만 지원합니다. 기기용은 `hmjec --target ios-app`으로 빌드하며, Apple 개발자 인증서와 프로비저닝 프로필을 `HMJE_IOS_SIGNING_IDENTITY`, `HMJE_IOS_PROVISIONING_PROFILE`로 지정합니다.

### Windows 11
- **LLVM clang (필수, 패키지에 없음):** `winget install LLVM.LLVM`
- **MSVC Build Tools + Windows SDK (필수):** `winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`
- **설치 후:** 툴체인의 `Install.cmd`를 다시 실행하면 clang 위치를 기억합니다. 또는 `C:\Program Files\LLVM\bin`을 PATH에 추가하거나 `HMJE_WINDOWS_CLANG`에 `clang.exe` 경로를 지정한 뒤 IDE를 다시 실행하십시오.
- **Git (선택):** IDE의 Git 기능. `winget install Git.Git`

### Linux (x86_64 · arm64)
- **glibc 2.38 이상:** 예) Ubuntu 24.04, Debian 13, Fedora 39 이상. 더 오래된 배포판에서는 `GLIBC_2.38 not found`로 실행되지 않습니다.
- **clang · OpenSSL 3 · libX11 (필수):** OpenSSL 개발 헤더가 없으면 HmjeNetwork와 이를 쓰는 HmjeUI 앱 빌드가 실패합니다. 화면은 X11 또는 XWayland가 필요합니다.
  - Ubuntu/Debian: `sudo apt install clang build-essential libssl-dev libx11-dev`
  - Fedora: `sudo dnf install clang gcc openssl-devel libX11-devel`
- **한국어 입력:** IBus 또는 Fcitx 한글 엔진과 UTF-8 로캘.
- **선택 기능:** 파일 선택 창은 xdg-desktop-portal 또는 zenity, 자격 증명 저장은 libsecret과 D-Bus 세션.

### Android (선택 · macOS, Windows, Linux x86_64)
- **Android SDK · NDK · JDK:** Android Studio를 설치하고 SDK Manager에서 NDK와 build-tools를 추가하십시오. 위치는 `ANDROID_SDK_ROOT`(또는 `ANDROID_HOME`), `ANDROID_NDK_ROOT`나 표준 설치 위치에서 찾습니다.
- **IDE에서 실행:** platform-tools(adb), emulator, 가상 기기(AVD). Linux arm64에서는 Android 빌드를 지원하지 않습니다.

## 시작하기

[Beta 1 다운로드 안내](README.md#설치)에서 호스트에 맞는 패키지를 내려받고 동봉된 설치 안내를 따르십시오.
[README](README.md), [언어](docs/language.md), [문법](docs/grammar.md),
[릴리스 기반 소스 빌드](docs/building.md)를 이어서 읽을 수 있습니다.
