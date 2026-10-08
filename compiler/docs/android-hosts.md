# 데스크톱 호스트에서 Android 빌드

macOS·Linux x86_64·Windows 네이티브 툴체인은 `android-llvm-ir`, `android-exe`, `android-apk`를 빌드합니다. Android 산출물의 ABI는 **arm64-v8a**이며 `--target-arch x86_64`는 거부합니다. 빌드 호스트의 아키텍처와 Android 대상 ABI는 별개입니다. macOS에서는 Xcode로 iOS 산출물도 빌드할 수 있습니다.

## Windows 빌드 환경

APK 빌드에는 Android SDK의 플랫폼·빌드 도구, NDK와 `java`·`javac`·`jar`·`keytool`을 포함한 JDK가 필요합니다. SDK 경로는 `ANDROID_HOME`, `ANDROID_SDK_ROOT`, `%LOCALAPPDATA%/Android/Sdk` 또는 `%USERPROFILE%/AppData/Local/Android/Sdk` 순으로 찾습니다.

`ANDROID_NDK_ROOT`와 `ANDROID_NDK_HOME`은 SDK의 `ndk` 디렉터리보다 우선합니다. 명시한 경로가 없으면 필요한 도구를 포함한 설치 버전 중 숫자로 비교한 가장 높은 버전을 선택합니다. Windows ARM64에서도 NDK 호스트 도구는 `toolchains/llvm/prebuilt/windows-x86_64`에 있으며, 컴파일러는 Android 대상 트리플을 지정하여 `clang.exe`를 실행합니다.

JDK는 `JAVA_HOME`으로 지정합니다. 지정하지 않으면 Android Studio의 `%ProgramFiles%/Android/Android Studio/jbr`을 먼저 확인하고, 없으면 PATH에서 찾습니다. SDK의 D8과 APK 서명 도구는 JAR을 직접 실행하여 공백이 있는 인자를 보존합니다. 클래스와 APK 내용은 JDK의 `jar`로 묶으므로 Windows에서 POSIX의 `rm`·`sh`·`find`·`xargs`·`zip`은 필요하지 않습니다. Windows 툴체인 패키지에는 선언된 Java·AAR 빌드용 Android Maven 입력이 포함됩니다.

```powershell
hmjec --project . --artifact MyApp --target android-apk -o "build/My App.apk"
```

## 대상 버전과 서명

매니페스트의 Android 최소 버전은 컴파일 대상 API를 정하며 최소 API는 21입니다. `HMJE_ANDROID_TARGET_SDK`는 이 최소 버전 이상이어야 하며, `HMJE_ANDROID_VERSION_CODE`는 양의 정수여야 합니다.

디버그 서명은 `%USERPROFILE%/.android/debug.keystore`를 사용합니다. 릴리스 서명에는 `HMJE_ANDROID_KEYSTORE`, `HMJE_ANDROID_KEY_ALIAS`, `HMJE_ANDROID_KEYSTORE_PASSWORD`가 필요하며 `HMJE_ANDROID_KEY_PASSWORD`는 선택 사항입니다.

필수 도구가 없거나 SDK 도구가 실패하면 0이 아닌 종료 코드를 반환합니다. 하위 도구가 성공을 반환했더라도 산출물이 없으면 실패로 처리하며 SDK 진단을 오류에 포함합니다.

Android SDK·NDK 설치와 실제 기기 실행 환경은 각각 준비해야 합니다. Android 빌드는 macOS 소스 동기화 검증에 포함되지 않았습니다.
