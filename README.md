# C HTTP Lab

C11과 POSIX 소켓으로 구현한 정적 웹 서버 + 리버스 프록시입니다. HTTP 라이브러리나 프레임워크를 사용하지 않습니다. 앞단과 두 백엔드 모두 **연결당 자식 프로세스 하나, 요청 하나, 응답 후 close** 방식입니다. 구현 요구사항은 [DEVELOPMENT.md](DEVELOPMENT.md)에 있습니다.

## 빠른 시작

macOS의 Xcode Command Line Tools 또는 Linux의 C 컴파일러와 Make가 필요합니다. 테스트에는 Python 3.8 이상, 부하 측정에는 `ab`가 필요합니다. 서버 자체에는 Python이 필요하지 않습니다.

이 README가 있는 저장소 루트에서 빌드합니다.

```sh
make
```

서로 다른 터미널에서 각각 실행합니다.

```sh
./echo_backend 8001
```

```sh
./time_backend 8002
```

```sh
./webserver server.conf
```

브라우저에서 **http://127.0.0.1:8080/** 을 엽니다. Echo 입력창의 `안녕`을 전송하면 `전송 6바이트 · 응답 6바이트 · 바이트 일치 ✓`가 나옵니다. ‘시각 가져오기’는 UTC 시각을 표시합니다. 각 서버는 Ctrl+C 또는 SIGTERM으로 종료합니다.

| 요청 | 처리 | 성공 응답 |
|---|---|---|
| `GET /`, `GET /index.html` | 앞단의 `www/index.html` | HTML 원본 바이트 |
| `POST /api/echo` | Echo 백엔드 | 입력과 같은 바이트, NUL 포함 가능 |
| `GET /api/time` | Time 백엔드 | `2026-11-20T06:01:07Z` 형태의 UTC 텍스트 |

## 설정

```text
listen 127.0.0.1 8080
root ./www
route /api/echo localhost 8001
route /api/time localhost 8002
```

`listen`과 `root`는 각각 하나, `route`는 최소 하나가 필요합니다. 하나의 경로를 두 번 등록할 수 없습니다. 포트 범위는 1~65535입니다. 필수 항목 누락은 항목명으로, 문법·중복·포트 오류는 줄 번호와 함께 시작 시 보고합니다. 루트 디렉터리를 열 수 없어도 시작하지 않습니다.

한 줄에 한 지시어를 쓰고 토큰은 공백으로 구분합니다. 빈 줄과 첫 비공백 문자가 `#`인 주석 줄을 허용합니다. 따옴표·인라인 주석은 지원하지 않습니다. 폴더 이름에 공백이 있다면 공백을 포함하지 않는 상대 경로를 사용하세요. **상대 root는 실행 위치가 아니라 설정 파일의 디렉터리 기준**입니다. 설정 파일 자체의 경로에 공백이 있는 것은 괜찮습니다.

`listen`의 주소는 설정대로 바인딩합니다. 백엔드는 각각 `127.0.0.1`에만 바인딩합니다. route의 호스트는 `getaddrinfo`로 해석하며 IPv4/IPv6 연결 후보를 순서대로 시도합니다. 경로는 쿼리를 제거한 뒤 설정과 정확히 비교합니다. 예를 들어 `/api/echo?q=1`은 Echo로 가고 `/api/echo/extra`는 정적 파일 처리로 갑니다. 백엔드는 자신의 `/api/echo`, `/api/time` 경로를 검사하므로 기본 매핑을 바꾸면 해당 백엔드의 경로도 함께 맞춰야 합니다.

## HTTP 계약과 제한

HTTP/1.0과 HTTP/1.1의 origin-form 요청을 받습니다. HTTP/1.1은 비어 있지 않은 Host 하나가 필수입니다. 헤더 이름은 대소문자를 구분하지 않습니다. 지속 연결과 파이프라이닝은 제공하지 않으며 항상 `Connection: close`를 보냅니다. 길이가 명시된 POST를 지원하고 0바이트도 허용합니다. `Transfer-Encoding`은 명시적으로 거절합니다.

| 조건 | 응답 |
|---|---|
| 잘못된 문법·버전·Host·길이, 중복 Content-Length, 청크 전송 | 400 |
| 지원하지 않는 리소스 메서드 | 405 + Allow |
| 클라이언트 요청 수신 제한 시간 초과 | 408 |
| POST 길이 누락 | 411 |
| 본문·URI·헤더 한도 초과 | 413 / 414 / 431 |
| 접근 금지 / 파일 없음 / 내부 오류 | 403 / 404 / 500 |
| 백엔드 연결 실패·잘못된 응답 / 소켓 시간 초과 | 502 / 504 |
| HTTP/1.1에서 지원하지 않는 Expect | 417 |

HEAD는 지원 메서드가 아니므로 405를 보내되 본문은 보내지 않습니다. 프록시는 HEAD 및 204/304의 본문 없는 응답 경계도 처리합니다. HTTP/1.1의 `Expect: 100-continue`는 길이·헤더 검증 후 처리합니다. HTTP/1.0에서는 Expect를 무시합니다. 일반 백엔드 응답에는 Content-Length가 필요하며 길이가 모자라거나 프레이밍이 모호하면 502입니다.

실제 상수는 [include/server.h](include/server.h)에 있습니다.

| 상수 | 실제 값 / 적용 범위 |
|---|---|
| `HEADER_LIMIT` | 16,384바이트, 요청/상태 라인과 마지막 CRLF까지 포함 |
| `HEADER_COUNT_LIMIT` | 앞단 요청·백엔드 응답의 헤더 필드 100개 |
| `URI_LIMIT` | 쿼리 포함 요청 대상 2,048바이트 |
| `BODY_LIMIT` | 요청 본문·일반 백엔드 응답 본문 각각 1,048,576바이트 |
| `PROXY_HEADER_LIMIT` | 재구성 요청 버퍼 17,408바이트; 번들 백엔드 수신에도 적용 |
| `BACKEND_HEADER_COUNT_LIMIT` | 번들 백엔드 요청 필드 103개; 프록시가 추가하는 헤더 공간 확보 |
| `ROUTE_LIMIT` | 경로 매핑 최대 32개 |
| `HOST_LIMIT` / `PATH_LIMIT` | 설정 호스트 255바이트 / 루트 경로 버퍼 4,096바이트 |
| `IO_TIMEOUT_MS` | 소켓 I/O 단계별 5,000ms |
| `SHUTDOWN_GRACE_MS` | 정상 종료 대기 6,000ms 후 남은 자식에 SIGTERM, waitpid |
| `LISTEN_BACKLOG` | 128; 커널의 실제 backlog 제한도 적용됨 |
| `FILE_CHUNK_SIZE` | 정적 파일 전송 버퍼 65,536바이트 |

요청 전체(헤더+본문), backend connect, 백엔드 요청 송신, 백엔드 응답 전체, 응답 송신에 각각 제한 시간이 적용됩니다. 부분 읽기/쓰기가 이어져도 같은 단계의 마감 시각을 갱신하지 않습니다. 이는 모든 처리가 5초 안에 끝난다는 뜻은 아닙니다. 소켓은 nonblocking으로 설정하고 `poll`로 기다려 **자식 한 개의 흐름은 동기적으로 처리**합니다. 부모는 요청 본문을 기다리지 않습니다.

**DNS 제한:** `getaddrinfo`는 시스템 리졸버의 동기 API이며 소켓의 5초 마감 이전에 실행됩니다. 외부 DNS 조회가 지연되면 OS의 리졸버 제한 시간까지 기다릴 수 있습니다. 수업 시연과 재현 측정에는 기본 `localhost` 또는 숫자 루프백 주소를 사용합니다. 이름 해석 실패는 502, connect/send/recv 마감 초과는 504입니다.

정적 파일에는 BODY_LIMIT를 적용하지 않고 파일 크기로 Content-Length를 정해 전송합니다. `openat`으로 디렉터리를 하나씩 따라가며 `O_NOFOLLOW`, `fstatat`, `fstat`으로 루트 밖 이동, 심볼릭 링크와 일반 파일 이외의 접근을 거부합니다. 경로의 percent escape를 한 번 디코딩하고 `.`/`..`, NUL, 역슬래시 등을 차단합니다. 디렉터리 목록은 제공하지 않습니다. PNG/JPG를 포함한 파일은 문자열 함수로 크기를 계산하지 않습니다.

프록시는 원래 메서드·요청 대상(쿼리 포함)·본문과 연결에 종속되지 않는 헤더를 전달합니다. Host, Content-Length와 Connection은 새로 구성하고 Connection이 지목한 헤더 및 고정 hop-by-hop 헤더를 제거합니다. 응답은 BODY_LIMIT 안에서 전부 검증한 다음 상태·Content-Type·본문과 필요한 Allow를 반환합니다. 응답을 보내기 시작한 후 파일 읽기나 클라이언트 송신이 실패하면 연결을 닫습니다.

캐시, 라운드로빈, 청크 전송, TLS, HTTP/2, 지속 연결은 현재 범위에 없습니다. 동시 자식 수의 애플리케이션 제한은 두지 않았으므로 많은 느린 연결은 OS 프로세스·FD·메모리 제한에 도달할 수 있습니다.

## 테스트

```sh
make test       # 단위 테스트 + 실제 TCP 통합 테스트 21개
make sanitize   # ASan + UBSan으로 다시 빌드하고 전체 테스트
make clean
make            # sanitizer 사용 후 일반 빌드 복구
make benchmark  # ab 측정 및 docs/validation.md 갱신
```

단위 테스트는 외부 도구가 필요하지 않습니다(`make unit`). 통합 테스트는 임시 설정·폴더·루프백 포트를 사용하며 서버를 직접 시작하고 종료합니다. FD 검사는 Linux `/proc`, macOS `lsof`를 사용하고 프로세스 검사는 `ps`를 사용합니다. 직접 띄운 기본 포트의 서버와 분리되어 실행됩니다. 로컬 소켓 bind가 금지된 샌드박스에서는 해당 권한이 있는 환경에서 실행해야 합니다.

검증 항목은 헤더/본문 동시 수신과 분할 수신, 짧은 본문 뒤 EOF, 최대 1MiB Echo, 한도 초과, 헤더 프레이밍, MIME·정적 파일 원본 일치, symlink·인코딩된 traversal·FIFO·Unix 소켓 차단, 느린 클라이언트와 병렬 GET, 백엔드 중단·불완전한 응답·시간 초과, 요청/응답 재구성, HEAD/204/304, 종료 후 FD·자식·리스너 회수입니다.

수동 확인은 서버 세 개를 실행한 뒤 진행합니다.

```sh
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/api/time
curl -i --data-binary '안녕' http://127.0.0.1:8080/api/echo
curl -sS http://127.0.0.1:8080/sample.png -o /tmp/http-sample.png
cmp www/sample.png /tmp/http-sample.png
curl -sS http://127.0.0.1:8080/sample.jpg -o /tmp/http-sample.jpg
cmp www/sample.jpg /tmp/http-sample.jpg
```

Echo 백엔드 터미널에서 Ctrl+C를 누른 뒤 Echo는 502, Time과 정적 GET은 계속 200인지 확인합니다. 지연 백엔드를 통한 504는 통합 테스트가 검증합니다.

## 실제 검증 결과

2026-11-20 기준입니다. [실측 보고서](docs/validation.md)에 환경·실행 명령·FD 결과를 기록했습니다.

- macOS 15.7.7 x86_64 / Apple Clang 17: `make test` 단위 테스트와 통합 테스트 21개 통과.
- 같은 macOS에서 `make sanitize` 전체 통과. ASan/UBSan 진단 없음. 자식은 `_exit`하므로 자식의 종료 시 leak 검사에 의존하지 않으며, 메모리는 코드에서 해제하고 FD·프로세스 회수를 별도로 검사합니다.
- 로컬 Linux 개발 컨테이너에서도 `make clean && make test` 전체 통과. 소스는 읽기 전용으로 연결하고 컨테이너 내부 복사본에서 검사했습니다.
- Chrome: HTML·CSS·JS 로드, ‘안녕’ 6바이트 일치, UTC 시각 표시 확인. curl로 상태·길이·본문과 PNG/JPG 원본 일치 확인.
- 반복 중도 종료 뒤 부모 FD 유지, 좀비 없음. SIGINT 종료 시 자식 종료 확인과 리스너 재바인딩 통과.

| ab 요청 | 요청 수 | 동시 접속 | 처리량(req/s) | 실패 / 비-2xx |
|---|---:|---:|---:|---:|
| 정적 GET | 1,000 | 1 | 493.59 | 0 / 0 |
| 정적 GET | 1,000 | 10 | 1,654.75 | 0 / 0 |
| 정적 GET | 1,000 | 50 | 3,228.52 | 0 / 0 |
| Echo POST | 500 | 10 | 1,254.68 | 0 / 0 |
| Time GET | 500 | 10 | 1,045.73 | 0 / 0 |

`ab` 기본 HTTP/1.0, keepalive 미사용입니다. 8 logical CPU의 루프백 단일 측정이며 최대 수용량을 뜻하지 않습니다. 정적 GET의 동시 접속을 1→50으로 늘렸을 때 처리량은 약 6.5배 늘었지만 평균 요청 시간도 2.026→15.487ms로 늘었습니다. 프록시 c=10은 정적 c=10보다 낮은 처리량을 보였습니다. 프록시 요청은 앞단 자식 외에 백엔드 연결과 백엔드 자식도 필요하기 때문입니다. 프로세스 생성과 문맥 전환, 느린 클라이언트가 차지하는 자원은 그대로 남습니다. 이벤트 루프 모델과 직접 비교한 실험은 하지 않았습니다.

측정 전후 부모 FD는 Echo 6→6, Time 6→6, Front 7→7이고 작업 종료 뒤 남은 자식은 0개였습니다. 세 부모의 SIGTERM 종료 코드는 모두 0이며 세 포트 재바인딩도 통과했습니다.

현재 결과는 작업 트리에 대한 검증입니다. **최종 병합 후에도 `make clean`, `make test`, `make sanitize`, 일반 빌드 복구, `make benchmark`를 같은 순서로 다시 실행**하고 브라우저 시연을 확인하세요.

## 코드에서 한 요청 따라가기

| 파일 | 책임 |
|---|---|
| `src/process.c` | accept/fork, 부모·자식 FD 소유권, signal pipe, waitpid, 종료 |
| `src/io.c` | 이름 해석, listen/connect, deadline 기반 부분 송수신 |
| `src/http.c` | 요청/응답 파서, 경계·길이 검증, 응답 직렬화와 로그 |
| `src/config.c` | 설정과 포트 검증, 상대 루트 해석, 정확한 라우팅 |
| `src/static.c` | 안전한 파일 열기, MIME, 정적 파일 송신 |
| `src/proxy.c` | 프록시 헤더 재구성, 백엔드 송수신과 오류 매핑 |
| `src/webserver.c` | 앞단 요청 분기와 main |
| `src/backend.c` | Echo/Time 실행 파일 공통 진입점과 각 처리 |

**정적 GET 한 건:** `webserver.c:main` → `config_load`가 root FD를 열고 `run_server` 호출 → `process.c:run_server`에서 accept 후 fork → 부모는 연결 FD를 닫고 자식은 listener와 signal pipe를 닫음 → `handle_frontend` → `read_request`/`parse_request_head` → `route_find`가 매핑 없음 확인 → `serve_static` → `open_static` → MIME과 fstat 크기로 `send_head`, read + `send_all_until` → 파일 close, `message_free` → `finish_client`와 `_exit` → 부모의 `reap_children`/waitpid.

**프록시 POST 한 건:** 같은 accept/fork 흐름 → `read_request`가 헤더와 이미 수신한 본문을 합쳐 Content-Length만큼 수집 → `route_find` → `serve_proxy` → `build_proxy_head` → `connect_backend`의 getaddrinfo/socket/connect로 별도 backend_fd 생성 → `send_all_until`로 헤더와 바이너리 본문 전달 → Echo의 `handle_backend`가 `read_backend_request` 후 그대로 `send_response` → 앞단의 `read_response`가 프레이밍과 본문 검증 → backend_fd close → 앞단이 상태·Content-Type·본문을 클라이언트에 송신 → 양쪽 `message_free`, 소켓 close, 자식 종료와 부모 waitpid.

stderr 로그는 `pid=... method=POST path=/api/echo destination=localhost:8001 status=200` 형태입니다. 앞단 자식 PID로 요청 하나를 추적하고 백엔드 로그의 endpoint로 다음 구간을 확인할 수 있습니다.

## CS:APP 11장 연결과 4분 시연

`io.c:listen_socket`의 socket/bind/listen은 수동 서버 소켓을 준비합니다. TCP 연결 수립은 커널이 처리하며 `process.c:accept`는 **이미 수립된 연결**을 애플리케이션의 연결 FD로 넘깁니다. accept 자체가 3-way handshake를 수행하는 것은 아닙니다.

`connect_backend`의 getaddrinfo는 이름 해석 API입니다. `localhost`는 로컬 hosts/리졸버 설정으로 해결될 수 있어 항상 외부 DNS 요청이 발생하는 것은 아닙니다. 프록시 자식은 client_fd 쪽에서는 서버, backend_fd 쪽에서는 클라이언트입니다. 이는 서로 다른 두 TCP 연결이고 FD도 별개입니다.

연결마다 fork하고 응답 후 close하는 것은 이번 구현의 선택이며 HTTP나 TCP가 강제하는 방식이 아닙니다. 소켓 준비 대기를 poll로 제한하더라도 자식은 한 요청을 동기적으로 처리하며, 프로세스 생성 비용과 메모리·FD 소비가 누적됩니다. 위 측정에서 얻은 처리량·지연과 함께 이 한계를 설명할 수 있습니다.

1. **0:00~0:45** — 서버 세 개 실행, 브라우저 주소와 정적 HTML/CSS/JS 표시.
2. **0:45~1:30** — `안녕` Echo의 6바이트 일치와 Time 시연.
3. **1:30~2:30** — 위 두 함수 경로와 프록시의 두 FD·두 역할 설명.
4. **2:30~3:15** — Echo 종료 후 Echo 502 / Time 200, 통합 테스트·sanitizer 결과 제시.
5. **3:15~4:00** — 부하 표·FD 회수·fork 모델 한계 설명하고 Ctrl+C로 정리.

HTTP 경계 처리 참고: [RFC 9112 §6.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3), [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html).
