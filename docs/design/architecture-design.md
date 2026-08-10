# project-atlas — 아키텍처 설계 문서

> 작성 2026-08-06 · 상태: **설계 확정, 구현 전**
> 이 문서가 프로젝트의 SoT다. 코드와 문서가 어긋나면 코드가 진실이고, 문서를 고친다.

---

## 1. 이 프로젝트가 무엇인가

**C++ MMO 게임서버 프레임워크.** 게임 하나가 아니라, **클라이언트만 갈아끼우면 어느 게임에도 적용 가능한 서버 코어**를 만든다. 데모 게임은 코어를 전부 행사(exercise)하기 위한 최소 구현이며, 그 자체가 목적이 아니다.

### 1.1 왜 만드는가 (우선순위 순)

1. **취업 산출물.** 게임 서버 직무의 요구 스택(C++ · 비동기 IO · TCP · 직렬화 · 멀티스레드 · 부하)을 실제 구현물로 증명한다. 기존 포트폴리오의 최대 공백은 **트래픽·실시간 부재**다 — 대규모 동접을 "봤다"에서 "만들었다"로 바꾸는 것이 이 프로젝트의 1차 목적이다.
2. **재사용 자산.** 이후 개인 게임 프로젝트가 이 프레임워크 위에 얹힌다.
3. 🔴 **매출은 목적이 아니다.** 이 프로젝트에 수익 기대를 걸지 않는다. 수익 트랙은 별도이며 이 문서의 범위 밖이다.

### 1.2 왜 하이퍼 미니멀 그래픽인가

1인 개발의 최대 병목은 아트다. 데모 게임의 렌더는 **도형 프리미티브 코드 온리**로 하고 텍스처를 쓰지 않는다. 이 방식은 자매 프로젝트(`project-roll`)에서 이미 검증됐다. 아트 병목이 사라지면 개발 시간 전량이 서버 코어로 간다.

부수 효과가 하나 더 있다: **게임 컨셉 자체가 스트레스 테스트가 된다.** "천 명이 한 보스를 같이 때린다"는 컨셉이 곧 동접 실증이고, 마케팅 문구와 기술 증명이 같은 문장이 된다.

### 1.3 🔴 실증이 유저 유입에 의존하지 않는다

"1000 CCU를 버텼다"는 **봇 클라이언트 1000개**로 증명한다. 실유저가 필요 없다. 이것이 이 프로젝트를 앞선 시도들과 가르는 지점이다 — 과거 출시작들의 실패 원인은 개발이 아니라 유통이었고, 이 프로젝트의 성공 판정은 유통을 경유하지 않는다.

---

## 2. Phase 1 성공 기준

Phase 1은 **2026-11-30**까지다(AX 지원사업 종료 시점). 아래가 전부 나오면 성공이고, 게임의 재미나 완성도는 판정 대상이 아니다.

| 항목 | 기준 |
|---|---|
| 동접 | 단일 WORLD에 **봇 1000 CCU** 유지 |
| 지연 | 서버 틱 처리 시간 **p95** — 1차 측정 후 목표치 역산 |
| 대역폭 | 클라 1대 수신 대역폭 상한 — AoI·델타·양자화 효과를 실측으로 제시 |
| 무중단 | **WORLD attach/detach 시 세션 무중단 재배치** |
| 정합성 | 패킷 로그 기준 **desync 0** |
| 산출물 | 부하 실측 리포트 + 공개 레포 + 기술 블로그 |

🔴 **공개 레포명은 `project-atlas`로 확정됐다** (2026-08-07 공개, `https://github.com/zzzz955/project-atlas`). 이미 푸시된 이후의 rename은 링크 · 스타 · 검색 인덱스를 전부 버리므로 되돌리지 않는다. **게임 타이틀은 레포명과 별개 항목이며 여전히 미결이다**(§16).

🔴 p95/대역폭의 구체 목표치를 지금 정하지 않는다. 1차 부하 측정 전의 숫자는 근거 없는 추측이고, 근거 없는 목표는 나중에 조용히 낮춰진다.

---

## 3. 범위

### 3.1 Phase 1 (~2026-11-30) — 서버 코어

- 서버 3종: **FE · GAME · WORLD** (전부 C++/Boost.asio)
- 서버 코어 15항목 (§4)
- 데모 게임 최소 집합 (§3.3)
- 봇 클라이언트 + 부하 측정 하네스
- 전체 토폴로지 설계 문서 (미구현 서버 포함 — 설계 역량 증명은 구현 없이도 성립한다)

### 3.2 Phase 2 (2026-12~) / Phase 3 (2027-01~02)

- **Phase 2**: Admin(웹) + Admin HTTP 요청의 무결성·원자성 계층 · 게임 콘텐츠 확장 · **기존 게임 1종을 이 프레임워크에 이식**(← 템플릿화의 진짜 검증)
- **Phase 3**: Relay · Matching · InterWorld 실구현 · 출시

### 3.3 데모 게임 최소 집합 — 🔴 절대 늘리지 않는다

- 공유 필드 1개(그리드 좌표) · 도형 캐릭터
- 이동 · 기본공격 1 + 스킬 2
- 몹 스폰 + **공유 보스 1종**
- 성장 축 1개
- 채팅 (브로드캐스트 경로 증명용, 저비용)

이 목록에 항목을 추가하려면 **서버 코어의 어떤 미검증 경로를 행사하기 위해서인지**를 먼저 답해야 한다. 답이 없으면 추가하지 않는다.

### 3.4 비범위 (Phase 1)

퀘스트 · 스토리 · NPC 대화 · 장비 슬롯 다중화 · 직업 다수 · 오픈월드 · PvP · 결제 · 클라이언트 UI 완성도

🔴 **CD(배포 자동화)도 비범위다** (2026-08-07 확정). 레지스트리 푸시 · 무중단 롤아웃 · 배포 파이프라인은 Phase 1에서 만들지 않는다. 배포는 `compose`로 로컬/단일 호스트 기동까지이며(§15.3), CI 게이트(§15.4)까지만 자동화한다.

---

## 4. 서버 코어 15항목 (= 포트폴리오 자산 목록)

1. **커스텀 바이너리 프로토콜** — 헤더 최소화 · 비트패킹 · 좌표 양자화 · 스냅샷 델타 · AoI 필터링
2. **직렬화/역직렬화 코어** — `pkt_generator` C++ 타깃 확장 (C# / GDScript / C++ 계약 한 벌)
3. **무결성 3층** — 프레이밍 체크섬 / 세션키 HMAC / 서버 권위 검증
4. **시퀀스 넘버** — 중복·순서·재전송 검출
5. **strand 기반 세션 직렬화** — 동시성 단일 모델
6. **스레드 역할 분리** — I/O 풀 · 서비스(WORLD 틱) · DB 풀
7. **ctx 원장 인계** — trace id + user id + character id + 트랜잭션 상태, 스레드 경계 무손실
8. **커스텀 ORM** — schema.json → C++ CRUD/prepared 생성, 트랜잭션 RAII, per-character lock
9. **JWKS 오프라인 검증** — OpenSSL, 키 캐시·롤오버
10. **WORLD 동적 attach/detach** — 레지스트리 · 하트비트 · ini role 식별 (= InterWorld 동형)
11. **Actor / AoI 모델**
12. **BT(행동 트리) 엔진**
13. **서버 치트** — 환경 게이트
14. **패킷 로그** — 링버퍼 · 필터 · 덤프 · 리플레이
15. **빌드 체인** — CMake · PCH · unity build · vcpkg · Docker

---

## 5. 서버 토폴로지

```
[Godot PC 클라]  --커스텀 바이너리 / TCP-->  [FE (C++)]
                                              │  WORLD 레지스트리 · 세션 라우팅 · 하트비트
                                              │  (Relay 자리 = WORLD간 직접통신 금지)
                        ┌─────────────────────┴─────────────────────┐
                        │                                           │
                 [GAME (C++)]                              [WORLD (C++)] × N
                 영속 · 인벤 · 성장 · 우편                   좌표 · 전투 · 이벤트 · 룸 · AoI
                 커스텀 ORM → MySQL                          (= InterWorld, ini role 식별)
                        │                                    attach / detach 가능
                        │
                 [platform-auth (C#, 기존 자산)]  ← HTTP / JWKS
```

### 5.1 서버 역할

| 서버 | 언어 | 수명 | 역할 |
|---|---|---|---|
| **FE** | C++ | 상주 | 연결 종단 · 패킷 라우팅 · WORLD 레지스트리 · 하트비트 · 외부 노출 최소화 |
| **GAME** | C++ | **고정** | 영속 데이터 · 캐릭터 · 인벤 · 성장 · 우편 · ORM 소유 |
| **WORLD** | C++ | **동적 attach/detach** | 좌표 · 전투 · 이벤트 · 룸 · Actor · AoI · BT · 틱 루프 |
| Lobby | — | — | 별도 서버를 두지 않는다. 인증은 platform-auth(HTTP), 서버·캐릭터 선택은 GAME이 담당 |
| Relay / Matching / InterWorld | C++ | Phase 3 | 전서버 콘텐츠 |

### 5.2 🔴 확장 지점을 지금 못 박는다

Relay/Matching/InterWorld를 MVP에서 만들지 않지만, **붙을 자리는 지금 설계에 새긴다.**

- **WORLD ↔ WORLD 직접 통신 금지.** 서버 간 통신은 전부 FE 경유 또는 (Phase 3) Relay 경유. 이 규칙을 지금 지키면 Relay 도입이 배선 작업이 되고, 어기면 Phase 3에서 전면 재설계가 된다. 🔴 이 금지는 **전송 수단과 무관하다** — TCP 직결뿐 아니라 메시지 브로커를 경유한 우회도 같은 위반이다. Redis pub/sub 이 그 대표 우회로이며, 판정과 근거는 **§10.2**에 있다.
- **WORLD와 InterWorld는 동일 바이너리.** 자기 정체성은 ini 설정으로 식별한다(`role=world` / `role=interworld`, `world_id`, `server_group`). 즉 InterWorld는 신규 서버가 아니라 **WORLD의 설정 변형**이다. 그 ini가 무엇을 담고 무엇을 담지 않는지는 **§5.4**가 정한다.
- **GAME은 고정, WORLD는 유동.** WORLD는 기동 시 FE에 등록하고 하트비트를 보낸다. 이탈하면 FE가 세션을 재배치한다. 이 성질이 Phase 1 성공 기준의 "무중단 재배치" 항목이다.

### 5.3 재사용 판정

| 계층 | 상태 | 판정 |
|---|---|---|
| 인증(계정 · 게스트 앵커 · 업그레이드 · pid 체이닝) | `platform-auth` (C#) 가동 중 | **재사용.** 새로 만들지 않는다 |
| 데이터 파이프라인 규약 (`db_generator` / `pkt_generator` / `info_generator`) | 자매 프로젝트에서 검증됨 | **계승 + C++ 타깃 추가** |
| 서버 치트 2계층 게이트 설계 | `project-roll` ADR-0067 | **계승** |
| GAME 서버 본체 | ASP.NET Core 템플릿 존재 | 🔴 **재사용하지 않는다.** C++로 새로 만든다 — 그것이 이 프로젝트의 목적이기 때문이다 |

### 5.4 설정 소스 — ini 와 .env 를 섞지 않는다

§5.2가 `role` / `world_id` / `server_group` 을 ini로 식별한다고 못 박았으므로, 그 ini를 도입하면서 **경계도 같이 못 박는다.** 설정 소스는 2개이고, 서로의 영역을 침범하지 않는다.

| 소스 | 담는 것 | 특성 |
|---|---|---|
| `server/server.ini` (커밋됨) | `role` · `server_id` · `world_id` · `server_group` · 포트 · IO 워커 수 · 로그 레벨/보존 | 게임·배포별 변형. 코드 리뷰 대상 |
| `.env` (🔴 커밋 금지, `*.example` 만) | DB/Redis 호스트·자격증명 · JWKS URL · `ATLAS_ROLE` | 배포 환경별. 시크릿 |

🔴 **`ATLAS_ROLE` 은 표에서 유일한 예외이자 중복이다.** 컨테이너는 ini를 읽는 코드가 돌기 **전에** 어느 바이너리를 exec 할지 정해야 하므로, 배포층의 선택은 환경변수일 수밖에 없다(§15.3). 그래서 `.env` 의 `ATLAS_ROLE`(= 실행할 바이너리)과 `server.ini` 의 `[server] role`(= 런타임 정체성)은 **같은 값이어야 한다.** 어긋나면 GAME 바이너리가 WORLD 설정을 들고 뜬다. 대안(role을 ini에서만 읽기)은 이미지에 구워진 ini가 하나뿐이라 role별 컨테이너를 구분할 수 없어 성립하지 않는다.

🔴 **필수 환경변수에 조용한 기본값을 두지 않는다.** `compose.yaml` 은 필수 값을 전부 `${VAR:?메시지}` 로 읽는다 — 미설정/빈 값은 경고 뒤 빈 문자열이 아니라 **변수 이름이 찍힌 에러로 즉시 실패**한다(`build` 포함). `server/.env.example` 은 비-시크릿에 동작하는 로컬 기본값을 담고 비밀번호 2개만 비워 둔다.

섞으면 두 방향으로 터진다 — **시크릿이 ini를 타고 레포로 새거나**, 배포 환경이 바뀔 때마다 이미지를 다시 굽게 된다. 전자는 되돌릴 수 없는 사고이고, 후자는 컨테이너를 쓰는 이유 자체를 없앤다.

🔴 **로더는 시크릿 값을 로그에 찍지 않는다.** 로드 결과를 로그로 남길 때 `.env` 계열은 **키 이름까지만** 출력하고 값은 출력하지 않는다.

---

## 6. 아이덴티티 — 3계층

```
account            (platform-auth 소유. uid = 16자리 랜덤 long, pid = 24자 랜덤)
 └─ server         (world_group / server_id)
     └─ character  (GAME 서버 소유)
```

- **account는 platform-auth가 계속 소유한다.** 게스트 앵커 · upgrade-guest 409 메인라인 · pid 체이닝 등 기존 계정 규약을 그대로 계승하고, 이 프로젝트에서 재설계하지 않는다.
- **character는 GAME 서버 영속.** PK는 `(server_id, character_id)` 스코프. 한 계정이 **여러 서버에 여러 캐릭터**를 가진다.
- **접속 흐름**: FE 접속 → JWKS 검증(account 확정) → **서버 선택** → **캐릭터 선택** → WORLD 입장.
- 🔴 **per-user lock이 per-character lock으로 확장된다.** 같은 계정의 서로 다른 캐릭터는 동시 조작이 가능해야 하고, 같은 캐릭터의 중복 로그인은 막아야 한다. 잠금 단위를 계정으로 잡으면 전자가 깨지고, 세션으로 잡으면 후자가 깨진다.

---

## 7. WORLD 모델

### 7.1 Actor

```
Actor (기반)        — id · 좌표 · 상태 · AoI 등록 · 틱 대상
 ├─ PlayerActor     — 세션 바인딩 · 입력 구동 · 세션 끊기면 유예 후 제거
 └─ NpcActor        — 서버 AI 구동 · 스폰/리스폰 소유
```

AoI 그리드는 **Actor 단위**로 동작한다. 따라서 브로드캐스트 경로가 플레이어와 NPC를 구분할 필요가 없고, 델타 스냅샷도 Actor 공통으로 처리된다. 이 통일이 코드량과 버그 표면을 동시에 줄인다.

### 7.2 AoI (관심 영역)

🔴 **AoI 없이 브로드캐스트하면 1000 CCU에서 즉사한다.** 전체 브로드캐스트는 O(n²) — 1000명이면 초당 100만 메시지가 된다. AoI 그리드 분할이 선택이 아니라 생존 조건이다.

- 월드를 고정 크기 셀 그리드로 분할
- Actor는 자기 셀에 등록, 이동 시 셀 이동
- 브로드캐스트 대상 = 자기 셀 + 인접 셀
- 셀 경계 진입/이탈 시 enter/leave 이벤트

### 7.3 BT (행동 트리)

- **코어** = BT 엔진 (Sequence · Selector · Condition · Decorator · Action + 블랙보드), 서버 틱에서 구동
- **게임** = 트리 **데이터**(`info_generator` 경유) + 게임별 커스텀 Action 노드 등록
- MVP 노드: `Idle` · `Wander` · `Chase` · `Attack` · `Leash`
- 서버 권위이므로 **결정론이 필요 없다** — 설계 자유도가 높다

🔴 **Leash(제자리 회귀)는 성능 항목이다.** 어그로 해제 + 원위치 복귀 + 체력 회복. 이것이 없으면 몹이 맵 끝까지 추적하며 **AoI 셀 경계를 계속 넘나들어 enter/leave 브로드캐스트가 폭증**한다. 게임 규칙처럼 보이지만 실제로는 부하 제어 장치다.

---

## 8. 프로토콜

### 8.1 프레임 구조

고정 헤더를 최소화한다. 길이 · opcode · 시퀀스 + 무결성 필드.

#### 헤더 폭 확정 (2026-08-10, portfolio-slice T2)

🔴 이 표가 SoT다. `§8.5` DTO 와이어 포맷이 **페이로드 내부**를 정하고, 이 표가 **페이로드 바깥**을
정한다. 둘은 같은 LE 규약을 쓴다.

```
 0        2        4                8               12
 +--------+--------+----------------+----------------+
 | length | opcode |      seq       |     crc32      |  payload (length bytes) ...
 +--------+--------+----------------+----------------+
   UInt16   UInt16      UInt32           UInt32
```

| 필드 | 폭 | 의미 |
|---|---|---|
| `length` | `UInt16` LE | **페이로드 바이트 수. 헤더는 세지 않는다** |
| `opcode` | `UInt16` LE | 계약이 정하는 메시지 식별자 |
| `seq` | `UInt32` LE | 세션별 송신/수신 각각 유지(`§8.3`). 역행·중복은 **연결 종료** |
| `crc32` | `UInt32` LE | `opcode + seq + payload` 에 대해 계산 |

- **고정 헤더 12바이트.** 엔디언은 `§8.5` 리틀엔디언 고정 그대로다
- 🔴 **`crc32` 는 프레이밍 무결성 전용이다** — `§8.2` 1층. 변조를 막지 못하고, 막는 척해서도 안
  된다. 이 한계는 코드 주석과 외부 문서 양쪽에 명시한다
- 🔴 **HMAC 8바이트 필드는 지금 넣지 않는다.** `§8.2` 2층은 세션키 수립을 요구하고 그것은
  `§12` platform-auth JWT 연동을 끌고 들어온다. **필드만 만들고 0으로 채우면 "있는 척"이 된다.**
  클라이언트가 아직 없으므로 헤더를 12 → 20바이트로 넓히는 비용은 **현재 0**이고, 그 사실을
  문서에 적는 쪽이 자리를 비워두는 것보다 정직하다. `§16` 미결 항목은 그대로 열려 있다

### 8.2 무결성 3층 — 🔴 각 층의 역할을 혼동하지 않는다

| 층 | 수단 | 막는 것 | 못 막는 것 |
|---|---|---|---|
| 1 | **체크섬** | 프레이밍 오류 · 우발적 손상 | 🔴 **변조** (공격자가 재계산하면 끝) |
| 2 | **세션키 HMAC** (SHA-256 절단 8B) 또는 **AEAD** (ChaCha20-Poly1305) | 페이로드 변조 | 정상 형식의 부정 요청 |
| 3 | **서버 권위 검증** | 부정 요청 (좌표·쿨다운·재화를 서버가 계산) | — |

🔴 **"체크섬으로 변조를 검출한다"는 서술은 틀렸다.** 알고리즘이 공개돼 있어 공격자가 페이로드를 바꾸고 체크섬을 다시 계산하면 그만이다. 게다가 TCP가 이미 체크섬을 한다. 체크섬의 역할은 **자체 프레이밍 무결성**이고, 변조 방지는 MAC이, 최종 방어는 서버 권위가 담당한다.

### 8.3 시퀀스 넘버

중복 · 순서 뒤바뀜 · 재전송 검출. 세션별 송신/수신 각각 유지.

### 8.4 대역폭 — 🔴 포맷보다 델타·양자화가 자릿수를 결정한다

커스텀 프로토콜로 헤더를 줄이는 것은 패킷당 몇 바이트다. WORLD 브로드캐스트의 대역폭은 다음 셋이 결정한다:

1. **좌표 양자화** — `float` → 고정소수점 정수(예: 1cm 단위 `Int16`)
2. **스냅샷 델타** — 직전 스냅샷 대비 변경분만
3. **AoI 필터링** — 볼 필요 없는 Actor는 애초에 안 보냄

🔴 **스냅샷/델타 개념을 프로토콜 설계 1일차에 넣는다.** 나중에는 못 끼운다.

### 8.5 고정 결정

- **엔디언 = 리틀엔디언 고정.** x86/ARM 모두 LE이므로 변환 비용 0. 네트워크 바이트 오더(BE) 관례를 따르지 않는다. 🔴 "정했다"를 명시하지 않으면 반드시 혼선이 난다.
- **패킷 구조체에 `#pragma pack` 금지.** 패딩·정렬이 컴파일러/플랫폼마다 다르다. `pkt_generator`가 **필드별 write/read를 생성**한다.
- **부동소수점을 프로토콜에 싣지 않는다.** 양자화된 정수만.

#### DTO 와이어 포맷 확정 (2026-08-06, wp6)

`pkt_generator` 의 C++ 타깃이 실제로 방출하는 값이다. 🔴 이 표가 SoT이고 생성 코드 상단 주석에
같은 내용이 박힌다 — 한쪽만 고치면 안 된다(`tools/pkt_generator/pkt_generator.js` 의
`LENGTH_PREFIX_CPP` 한 곳에서 폭이 나온다).

| 항목 | 확정값 | 근거 |
|---|---|---|
| **길이 프리픽스** | `UInt16` LE, 모든 가변 길이 필드 앞에 붙는다. 문자열은 **바이트 수**, 배열은 **원소 수**. 상한 65535, 초과 시 쓰기에서 **클램프** | 폭이 고정이어야 프레임이 나중에 붙어도 재해석이 필요 없다. 조용히 잘린 스트림보다 클램프가 낫다 — 호출자가 크기를 스스로 제한해야 한다 |
| **문자열 인코딩** | **UTF-8**. 종료 문자 없음, BOM 없음. 프리픽스는 코드포인트가 아니라 바이트를 센다 | 계약(C#)·클라(GDScript)·서버(C++) 3타깃이 공통으로 쓰는 유일한 인코딩 |
| **bool** | 정확히 1바이트, 쓰기는 0/1. 읽기는 0이 아닌 모든 값을 `true` 로 | `bool` 의 sizeof 는 구현 정의라 와이어 폭을 별도로 못 박아야 한다 |
| **enum** | 계약에 선언된 기반 타입 폭 그대로(`: byte` → 1바이트). 열거자 값은 생성 코드에 **명시적으로** 박는다 | 멤버를 중간에 끼워 넣었을 때 뒤쪽 와이어 값이 조용히 밀리면 안 된다 |
| **읽기 실패** | `bool Read(std::span<const Byte>&)` 의 `false`. 🔴 throw 하지 않는다(`§11.2a`). 실패 시 커서와 대상 DTO를 건드리지 않는다 | 잘린 패킷은 예외 상황이 아니라 정상적인 입력이다 |
| **길이 검증 순서** | 모든 부호 없는 뺄셈 **전에** 남은 크기를 검사. 배열/문자열은 선언된 길이를 **할당 전에** 남은 버퍼와 대조하고 `reserve()` 하지 않는다 | 프리픽스는 공격자가 정한다 — 거짓말하는 프리픽스가 거대 할당이 아니라 버퍼 부족으로 죽어야 한다 |
| **중첩 컬렉션** | 🔴 미지원. 생성기가 거부하고 SoA(배열들의 구조체)로 펴라고 안내한다 | `§8.4` 델타 압축이 원하는 배치와 같다 |

🔴 **이 노드가 만들지 않는 것: 프레임 헤더 · opcode 테이블 · 시퀀스 · 체크섬 · HMAC.** `§8.1` 의
프레임 필드 폭과 `§16` 의 HMAC/AEAD 가 미결인 채로 헤더를 지어내면 계약이 확정될 때 생성기와
생성물을 둘 다 재작업하게 된다. 지금 생성되는 것은 **DTO 뿐**이며, 프레임은 코어의 몫이다.

---

## 9. 스레드 모델

```
I/O 스레드 풀 (asio io_context)
      │  수신 → 프레임 파싱 → 무결성 검증
      ▼
세션 strand (직렬화)
      │
      ├─→ 서비스 스레드 (게임 로직 / WORLD 틱 루프)
      │
      └─→ DB 스레드 풀 (블로킹 격리)
                │
                └─→ 완료 시 원래 strand로 post
```

#### 구현 확정 (2026-08-06, wp5)

설계에 값이 없던 3가지를 코드가 정했다. `server/atlas/net/{io_runner,session,acceptor}.{h,cpp}`.

- **워커 스레드 수 기본값 = `std::thread::hardware_concurrency()`.** 그것이 0을 답하는 플랫폼에서는
  1로 폴백한다(워커 1개도 동작하는 서버다). `IoRunner(worker_count)` 인자로 덮어쓸 수 있고,
  개수는 생성 시점에 고정된다 — 풀은 리사이즈하지 않는다.
- 🔴 **graceful shutdown 순서 = `acceptor.Stop()` → 살아있는 세션 `Close()` → `runner.Stop()`.**
  `runner.Stop()`은 work_guard를 놓아 큐가 비면 `run()`이 돌아오게 할 뿐이므로, **읽기가 걸려 있는
  소켓 하나가 풀 전체를 붙잡는다.** 앞문을 먼저 닫는 것이 drain을 유한하게 만드는 조건이다.
  `~IoRunner()`는 이 순서를 지키지 않은 호출자용 **안전망**으로 `io_context::stop()`(큐 폐기) 후
  join한다 — 의도된 경로가 아니며 Stop()과 의미가 반대다.
- **쓰기 큐 정책 = 세션 strand 위의 FIFO `deque<vector<Byte>>`, `async_write` 1개만 in-flight,**
  완료 핸들러가 다음 항목을 시작한다. 페이로드는 `Send()` 시점에 복사한다(호출자가 임시 버퍼를
  넘길 수 있다). ~~🔴 **상한은 두지 않았다**~~ — 백프레셔 상한은 "메시지 1건"이 정의돼야 의미가
  있고, 메시지는 프레임 계층(§8)이 생기기 전까지 존재하지 않았다.
  **해소 (2026-08-10, §8.1 헤더 확정과 동시)**: 페이로드 상한 `kMaxPayload = 16 KiB`, 세션 쓰기 큐
  상한 `kMaxWriteQueueBytes = 1 MiB`. 🔴 **초과는 연결 종료**이며 조용한 드롭이 아니다 — 드롭은
  프로토콜을 조용히 깨뜨리고, 그 증상은 며칠 뒤 엉뚱한 곳에서 나타난다.
- 🔴 **`IoRunner` 워커는 `io_context::run()`을 try/catch로 감싸지 않는다.** 모든 핸들러 진입점이
  이미 `Guarded`를 통과하므로(§11.2b), 여기까지 예외가 올라온다는 것은 **가드가 빠진 핸들러가
  있다는 뜻**이다. 여기서 잡아 워커를 재시작하면 가드가 막으려던 결함을 정확히 그 형태로 숨긴다.
  프로세스를 죽게 두는 쪽이 누락을 시끄럽게 만든다.
- 🔴 **세션 계층에 락은 0개다.** 세션의 모든 멤버는 자기 `Strand`에 bind된 핸들러에서만 만진다.
  회귀 방지선은 `net_session_test.cpp`의 재진입 카운터 테스트(최대값 == 1)와
  `rg "std::mutex|std::lock_guard|std::unique_lock" server/atlas/net/` → 0 이다.

### 9.1 동시성 단일 모델 — strand

🔴 **동시성 제어를 strand로 단일화한다.** 세션 상태는 strand로 직렬화되므로 락이 필요 없다.

- 락은 **진짜 공유 자원**(전역 설정 캐시, 커넥션 풀)에만 `shared_mutex`
- **세마포어는 뮤텍스 대체재가 아니다** — 소유권 개념이 없고(다른 스레드가 release 가능), 재귀 불가, 일반적으로 mutex보다 느리다. 정당한 용도는 **스레드 간 완료 신호 / 풀 크기 제한**뿐이다.
- 🔴 strand 위에 락을 또 얹으면 동시성 모델이 두 벌이 되고, 데드락은 정확히 그 교차점에서 난다.

*(Admin(웹) HTTP 요청의 무결성·원자성 보장은 Phase 2 항목이며, 그 계층에서 세마포어의 본래 용도가 검토된다.)*

### 9.2 ctx 원장

요청 진입점에서 ctx를 생성하고, **스레드 경계를 넘을 때마다 그대로 이동**한다. 담는 것: trace id · account/character id · 트랜잭션 상태.

🔴 **ctx를 단순 `thread_local`에 두면 안 된다.** strand는 실행 스레드를 고정하지 않으므로 핸들러마다 스레드가 바뀐다. 핸들러 가드(`Guarded`, §11.2)가 **진입 시 설치 / 종료 시 복원**하는 RAII 방식이어야 정확하다.

---

## 10. 영속 계층 — 커스텀 ORM

**MVP 범위를 못 박는다.** ORM은 범위가 폭발하는 대표 항목이다.

**포함**
- `schema.json` → C++ 구조체 + CRUD + prepared statement 바인딩 **생성** (`db_generator` C++ 타깃)
- 타입 세이프 명시 쿼리 API (🔴 문자열 SQL 조립 금지)
- 트랜잭션 스코프 **RAII** — 소멸자에서 commit되지 않았으면 **rollback**
- per-character lock (기존 ORM 규약 계승)

**금지 (Phase 1)**
- 관계 자동 로딩 · lazy loading
- change tracking / 유닛 오브 워크
- 마이그레이션 자동화

🔴 **트랜잭션 RAII와 예외 가드는 한 세트다.** `Guarded` 안에서 예외가 나면 트랜잭션이 자동 롤백돼야 "원자성 보장"이 성립한다.

### 10.1 슬라이스 경계 — 생성물 먼저, 실행 API는 ORM 런타임 노드 (2026-08-06 확정)

위 "포함" 목록은 **생성물 + 런타임 두 부분**이고, 이번 슬라이스(`db_generator` C++ 타깃)는 **생성물까지만** 만든다.

| | 내용 | 어디서 |
|---|---|---|
| 이번 슬라이스 | 테이블당 POD 행 구조체 · 컬럼 메타데이터(이름/타입/PK/null/길이) · **prepared statement 고정 문자열 상수**(`?` placeholder) · 파라미터 **바인딩 순서 배열** | `tools/db_generator/` → `server/generated/db/` |
| **후속 — ORM 런타임 노드** | 커넥션 · 트랜잭션 스코프 RAII · per-character lock · 타입 세이프 실행 API(CRUD) | 아직 없음 |

🔴 **런타임이 없는데 CRUD 실행 코드를 뽑으면 컴파일되지 않는다.** 그래서 순서가 이쪽이다 — 런타임 노드가 생기면 emitter를 확장해 CRUD 함수 시그니처까지 뽑고, 그 함수들이 이번에 뽑은 SQL 상수와 바인딩 배열을 그대로 쓴다. `?` 개수 == 바인딩 배열 길이는 생성 헤더의 `static_assert`가 강제한다.

🔴 **라이브 DB 마이그레이션 diff는 이식하지 않았다.** 원본(`project-tower`)의 db_generator는 `INFORMATION_SCHEMA`를 읽어 `ALTER` 문을 만들고 실행까지 했지만, ① 아직 실 DB가 없고 ② 공개 레포에 접속 자격 증명이 얽히면 안 된다. 남은 것은 원본 `--generate-only` 상당의 **오프라인 `schema.sql` 출력**뿐이며, 적용은 수동이다. `template.ini [db-gen]`에 접속 키가 없는 것도 같은 이유다. (마이그레이션 **자동화**는 위 "금지" 목록에도 이미 들어 있다.)

### 10.2 Redis — 자리만 새긴다 (2026-08-07 확정)

**Phase 1 성공 기준 6개(§2) 중 Redis를 요구하는 항목이 0개다.** FE 1개 · GAME 1개 고정 구성에서는 분산 락도 캐시 공유도 성립하지 않는다. 따라서 **C++ Redis 클라이언트 의존성을 추가하지 않는다** — `server/vcpkg.json` 은 6개 그대로다(§15.2). `compose.yaml` 에 서비스 정의만 두고 **기본 기동에서는 제외**한다(프로파일 뒤, §15.3).

🔴 **가장 중요한 항목은 pub/sub 금지다.** Redis pub/sub 은 §5.2의 **WORLD ↔ WORLD 직접통신 금지** Mandate를 우회하는 통로다. 그 규칙을 "TCP로 직접 연결하지 마라"로 좁게 읽으면, 두 WORLD가 Redis 채널로 대화하는 순간 규칙은 **문자적으로는 지켜지고 의미는 완전히 파괴된다.** 그렇게 되면 Phase 3의 Relay가 들어갈 자리가 사라진다 — 이 금지는 §5.2의 파생이며, 별도 규칙이 아니다.

| 역할 | 판정 | 근거 |
|---|---|---|
| 읽기 캐시 (전역 설정 · 랭킹 스냅샷) | 허용 | 프로세스 로컬로 못 하는 것만 |
| 분산 락 (GAME 다중화 시 per-character lock 확장) | 허용 | §10 per-character lock의 확장선 |
| WORLD 레지스트리 영속화 (FE 재시작 복구) | 조건부 허용 | 🔴 SoT는 계속 FE 인메모리. Redis는 복구용 사본 |
| 세션 상태 저장소 | 보류 | FE가 세션 종단이라는 §5.1이 흐려진다 |
| **서버 간 게임 트래픽 pub/sub** | 🔴 **금지** | §5.2 Mandate 우회 |

---

## 11. 로깅 · 예외

### 11.1 로그 — 매크로가 정당한 이유

```cpp
ATLAS_LOG_TRACE / DEBUG / INFO / WARN / ERROR / FATAL
```

`ATLAS_LOG_DEBUG("actor={} dump={}", id, ExpensiveDump(a))` 에서 로그 레벨이 꺼져 있으면 **`ExpensiveDump()`가 아예 평가되면 안 된다.** 함수로는 불가능하다(인자가 먼저 평가된다). 이것이 매크로의 유일하고 충분한 근거다. 릴리즈 빌드에서 TRACE/DEBUG를 `#if`로 통째 제거하는 것도 매크로만 가능하다.

- **구현 = spdlog 래핑.** ctx 주입 · 로그 정책 계층만 자체 구현한다. 로거 내부(포맷터/큐)는 차별화가 약한 영역이고, 아낀 시간이 AoI/프로토콜/ORM으로 가는 것이 맞다.
- 레벨 체크를 포맷팅보다 **먼저** 한다 (꺼져 있으면 문자열 생성 0회)
- `std::source_location`으로 파일/라인/함수 자동 첨부
- **ctx 원장 자동 주입** (trace id · session · character)
- 🔴 **파일 쓰기가 I/O 스레드를 블로킹하면 안 된다** — 큐 → 전용 로그 스레드 → 파일
- 롤링: 일자 + 보존 N일
- 🔴 **크래시/시그널 시 강제 flush** — 없으면 사고 직전 로그가 통째로 날아가 장애 분석이 불가능해진다
- 🔴 **패킷 로그는 별도 채널** — 용량이 자릿수로 크고 이진이라 텍스트 로그와 섞으면 둘 다 못 쓴다

#### 구현 확정 (2026-08-06, wp4)

- 🔴 **`log.h`는 spdlog를 include하지 않는다.** spdlog + fmt는 의존성 중 가장 무거운 헤더 집합인데
  `log.h`는 사실상 모든 TU가 포함한다. spdlog 표면 전체를 `log.cpp`의 out-of-line `LogWrite` 뒤로
  숨기고, 호출 지점 포맷팅은 `std::format`으로 한다 — 이것이 이 계층의 pimpl이다. CMake에서
  `spdlog::spdlog`를 **`PRIVATE`로 링크**하는 것이 그 은닉의 기계적 강제다. `PUBLIC`으로 바꾸면
  PCH·unity build로 번 시간을 그대로 반납한다.
- **레벨 게이트는 헤더의 `inline std::atomic<UInt8>`.** 매크로가 포맷팅 전에 정수 비교 하나만 하고
  빠져나오도록, 함수 로컬 static(매 호출 guard 검사)이 아니라 인라인 변수를 쓴다.
- **보존 일수 N = 14일**(`LogConfig::retain_days` 기본값). spdlog `daily_file_sink`가 자정에 롤링하고
  파일 개수 상한으로 보존을 처리한다.
- ⚠️ **크기 상한은 MVP에서 넣지 않았다.** spdlog에는 일자와 크기를 함께 보는 싱크가 없고
  (`daily_file_sink` = 일자+보존, `rotating_file_sink` = 크기+개수), 둘을 합치려면 보존 로직까지
  직접 재구현해야 한다. 필요해지면 `base_sink` 파생 싱크 하나로 추가한다 — 포맷터/큐를 건드리는
  일이 아니므로 §11.3의 "자체 구현 금지"에 걸리지 않는다.
- **크래시 flush 훅 = `std::signal` + `std::set_terminate`.** `SIGABRT · SIGSEGV · SIGILL · SIGFPE ·
  SIGINT · SIGTERM` 핸들러에서 flush → 기본 동작 복원 → 재발생(`std::raise`)시키므로 크래시 덤프와
  종료 코드는 그대로 남는다. `std::set_terminate`는 이전 핸들러를 체이닝한다. `SIGKILL`은 정의상
  잡을 수 없어 큐 꼬리를 잃는다 — 감수한다.
- **`LogCount(level)`** — 레벨별 누적 레코드 수. `Guarded`가 예외를 삼킬 때 "ERROR 로그가 정확히 1건"
  임을 테스트가 **관측**할 수 있게 하는 장치이고, 동시에 에러율 알람용 카운터다.

### 11.2 예외 정책

**(a) 예외는 예외적인 것에만.** 예상 가능한 실패(패킷 파싱 실패, 캐릭터 없음, DB 미스)는 예외가 아니다 → `std::expected` / `error_code` / `optional`. asio 자체가 `error_code` 오버로드를 제공하므로 그 결을 따른다. 예외는 **프로그래밍 오류 · 복구 불가**에만.

**(b) 🔴 비동기 핸들러 밖으로 예외가 나가면 스레드가 죽는다.** asio 핸들러에서 던진 예외는 `io_context::run()`까지 전파된다 → **I/O 스레드 1개 사망 = 그 스레드가 담당한 세션 전부 정지**. 조용히 서버가 반쪽 나는 최악의 장애 형태다.

따라서 필요한 것은 매크로 `try/catch`가 아니라 **핸들러 경계 가드**다:

```cpp
asio::post(strand_, atlas::Guarded(ctx, [self]{ ... }));
```

`Guarded`의 책임: ① ctx를 스코프에 설치(RAII) → 로그 매크로가 자동으로 읽는다 ② 모든 예외를 잡아 ERROR 로그 + 세션 안전 종료 ③ `noexcept` 보장.

**(c) 얇은 매크로 3개만**
```cpp
ATLAS_THROW(Type, fmt, ...)   // source_location + ctx 첨부하여 throw
ATLAS_CHECK(cond, fmt, ...)   // 실패 시 ATLAS_THROW. 릴리즈에도 살아있음
ATLAS_ASSERT(cond)            // 디버그 전용, 릴리즈에서 소거
```
🔴 순수 매크로로 `try/catch`를 감싸는 형태는 채택하지 않는다 — 중괄호 짝 · `return` 처리 · 중첩에서 지저분해지고 디버거가 따라가지 못한다.

### 11.3 락프리 큐 — 지금 만들지 않는다

큐가 필요해 보이는 두 자리에 이미 답이 있다:
- **로그 큐** → spdlog `async_logger` + `thread_pool` 내장
- **작업 큐** → `asio::post(strand, ...)` 자체가 큐잉

그리고 락프리 자체 구현은 리스크 대비 이득이 나쁘다:
- 🔴 **틀려도 대부분의 테스트를 통과한다.** ABA · `memory_order` 오지정 · 메모리 회수(hazard pointer/RCU). 버그가 나면 **재현 불가능한 형태**로 남고, 1000 CCU 부하 중 몇 시간에 한 번 터지는 크래시가 된다.
- **측정 전에는 병목인지도 모른다.** 조기 최적화다.
- 🔴 포트폴리오 관점에서도 손해다. *"왜 락프리인가"* → *"빠르니까"* 는 감점이고, *"프로파일링했더니 X 구간이 병목이라 거기만"* 은 가점이다. **측정 없는 락프리는 판단력 부족의 증거로 읽힌다.**

**경로**: ① MVP는 자체 큐 0개 → ② 1000 CCU 부하에서 프로파일링 → ③ 병목이 나오면 그 지점만 `boost::lockfree::spsc_queue`(Boost를 이미 쓰므로 추가 의존 0) → ④ 자체 구현할 자리가 있다면 **패킷 로그 링버퍼** 하나. 고정 크기 · **SPSC** · 오버플로 시 드롭. 🔴 **MPMC는 만들지 않는다.**

---

## 12. 인증 연동

platform-auth와 **HTTP**로 통신한다(Boost.Beast). 🔴 JWT/JWKS 검증은 C++로 직접 해야 한다:

base64url 디코드 → JWK 파싱 → `EVP_PKEY` 변환 → RS256/ES256 서명 검증 → claim/exp 검사 → **키 캐시 및 롤오버**. OpenSSL 직접 작업이며 작업량이 작지 않다.

**유지해야 할 성질**: 자매 게임들이 갖고 있는 *"JWKS 오프라인 검증이라 auth 장애에도 인증된 트래픽이 끊기지 않는다"* 를 C++에서도 그대로 유지한다.

---

## 13. 운영 도구

### 13.1 서버 치트

`project-roll` ADR-0067의 2계층 게이트 설계를 계승한다.
- 서버 측: 개발 전용 미들웨어가 **404**로 응답(존재 자체를 숨김)
- 🔴 **pid 화이트리스트 없음** — 환경(dev/prod)이 유일한 경계다
- 🔴 **로컬 폴백 금지** — 서버가 거부하면 클라가 자체 처리하는 경로를 두지 않는다

### 13.2 패킷 로그

커스텀 프로토콜 개발의 필수 도구이자, desync 디버깅 · 부하 분석 · 재현의 근거.
- 링버퍼(고정 크기, 오버플로 시 드롭)
- opcode 필터
- 덤프 / **리플레이**
- 🔴 텍스트 로그와 분리된 별도 채널

---

## 14. 템플릿화 — seam은 generator다

"클라이언트만 갈아끼우면 어느 게임에도" 가 성립하려면 교체 경계가 명확해야 한다. 경계를 **generator 3종**에 둔다.

| 층 | 내용 | 교체 |
|---|---|---|
| **코어 (고정)** | 네트워크 · 세션 · strand · 스레드 모델 · ORM 엔진 · 프로토콜 프레임 · HMAC · 시퀀스 · AoI · 틱 루프 · BT 엔진 · 로깅 · 예외 가드 · 치트 · 패킷 로그 | 하지 않음 |
| **계약 (생성)** | `pkt_generator` → 패킷/DTO · `db_generator` → 스키마/CRUD · `info_generator` → 정적 데이터 | 게임별 소스 교체 후 재생성 |
| **게임 (교체)** | 스탯 규칙 · 전투 판정 · BT 트리 **데이터** + 커스텀 Action 노드 · 맵 데이터 | 게임마다 새로 |
| **배포·스택 (설정)** | 소유 파일 = `server/server.ini` · `compose.yaml` · `server/.env.example`. role/포트/워커 수 · 스택 축 · 인프라 서비스 구성 — 두 소스의 경계는 **§5.4** | 게임·환경마다 값 교체 (🔴 코드 분기 없음) |

**게임 교체 = CSV / `schema.json` / contracts 교체 + 재생성 + ini / compose 교체.**

경계가 말로만 있으면 지켜지지 않으므로 **경로를 고정한다.** 입력·출력 경로와 네임스페이스는
`template.ini` 가 소유하고, 정규화 타입 → C++ 타입 매핑은 `tools/types.json` 의 `cpp` 열이 SoT다
(값은 `cpp-style.md §4.2` A안 별칭 — 🔴 `int`/`long` 금지).

| 생성기 | 입력 | C++ 출력 | ini 섹션 |
|---|---|---|---|
| `pkt_generator` | `shared/contracts/**/*.cs` | `server/generated/pkt/` | `[packet-gen]` |
| `db_generator` | `server/db/schema.json` | `server/generated/db/` (+ `schema.sql`) | `[db-gen]` |
| `info_generator` | `shared/datas/**/*.csv` | (데모 CSV 확정 후 — 생성기 자체가 아직 없다) | `[data-gen]` |

생성 네임스페이스는 `atlas::generated` 하나로 고정한다. 실행은 레포 루트에서 `npm run gen:all`,
드리프트 검사는 `npm run gen:check`(어긋나면 exit 1) — 🔴 **`server/generated/**` 는 전부 생성물이며
직접 편집하지 않는다.**

🔴 `gen:all` / `gen:check` 조성에는 **이 레포가 실제로 만드는 생성기만** 넣는다. 현재는 `db → pkt`
2단계이며 `gen:info*` 스크립트는 없다 — 아무도 쓰지 않을 생성기를 가리키는 스크립트를 남기면
`gen:check` 가 영구히 exit 1 이 되어 드리프트 게이트가 죽는다. `info_generator` 를 추가할 때 `package.json` 과 `tools/all_generator.bat`
양쪽에 `info` 단계를 **맨 앞에** 넣는다(입력 데이터가 스키마·패킷보다 앞선다).

이 방식의 이점은 **추측 기반 추상화를 만들지 않아도 된다**는 것이다(과설계의 주원인). generator 패턴은 자매 게임 3종에서 이미 검증됐다.

🔴 **정직한 한계**: 프레임워크는 **2개 이상의 게임에 붙여봐야 검증된다.** 1개만 만들고 "템플릿"이라 부르면 실제로는 잘못 그은 경계가 다수 남는다. **Phase 2에서 기존 게임 1종을 이 프레임워크로 이식하는 것**이 진짜 검증이며, 그때 비로소 "템플릿"이 주장이 아니라 사실이 된다.

컨벤션 자산(`.clang-format` · `.clang-tidy` · `.editorconfig` · `CMakePresets.json` · CI 워크플로 · `docs/conventions/cpp-style.md`)도 **프레임워크의 일부**다. 게임이 프레임워크를 가져가면 컨벤션 체계가 통째로 따라오고, "게임마다 스타일이 갈리는" 상황이 구조적으로 불가능해진다.

**배포·스택 층 — 축은 데이터로만 기술하고 분기는 구현하지 않는다 (2026-08-07 확정).**
"배치 파일 하나로 클라 엔진 / 인프라를 분기한다"는 **채택하지 않는다.** 근거는 셋이다.

- 분기가 실제로 갈리는 대상은 `server/vcpkg.json` · `CMakePresets.json` · `compose.yaml` · `tools/types.json` 의 타입 열 · `shared/contracts` 언어 · `.env.example` 키 집합, 이렇게 **6개 파일**이다. `setup.bat` 은 그 산물을 소비할 뿐이다. 배치에 `if` 를 넣으면 이 6개 파일의 진실이 배치 안에 **복제**되고, 그 복제본에는 `gen:check` 같은 게이트가 붙지 않는다.
- 클라 3종 × 인프라 2종 × 토폴로지 2종 = **12조합인데 게이트는 1조합만 돈다.** 나머지 11개는 조용히 썩는다.
- 위 "정직한 한계"가 이미 답했다 — 게임 하나도 붙여보지 않은 상태에서 스택 N종 지원을 주장할 수 없다.

따라서 **축은 `server.ini [stack]` 섹션에 데이터로 기술하고, 값은 현재 실측 1조합만 둔다.** 읽는 코드는 있고 `if` 는 없다.

```ini
[stack]
client = godot        ; 축은 존재, 값은 1개
server = cpp-asio
db     = mysql
cache  = none         ; redis 자리 (§10.2)
```

🔴 **승격 조건**: 실제로 **두 번째 조합이 생기고 그 조합이 CI 게이트에서 돌기 시작하는 시점**에, 이 축을 `stack_generator`(4번째 생성기)로 올린다. 조합이 1개인 동안 생성기를 먼저 만들면 §14가 경계한 "추측 기반 추상화"를 그대로 반복하는 것이다.

---

## 15. 빌드 체인

### 15.1 CMake + Unity Build + PCH

🔴 **Unity build와 CMake는 대립 관계가 아니다.** CMake는 빌드 시스템 생성기, unity build는 컴파일 기법(여러 `.cpp`를 한 TU로 합침)이다. 직교하며 **둘 다 쓴다.**

```cmake
set(CMAKE_UNITY_BUILD ON)
set(CMAKE_UNITY_BUILD_BATCH_SIZE 8)
set_source_files_properties(problem.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
```

**Linux/Docker에서도 동작한다.** unity build는 소스 연결일 뿐이라 컴파일러 무관이다. Windows 전용이 아니다(VS의 UI 체크박스가 그렇게 보이게 할 뿐이다).

**대가 4가지 — 전부 실재한다**
1. **증분 빌드가 나빠진다** — 한 파일을 고치면 배치 8개가 재컴파일. 개발 루프에서는 오히려 느릴 수 있다
2. **ODR/심볼 충돌** — 서로 다른 파일의 같은 이름 `static` 전역, 익명 네임스페이스, 파일 스코프 헬퍼
3. **매크로 누출** — 특히 `Windows.h`(`min`/`max`/`ERROR`)
4. 🔴 **누락된 `#include`를 숨긴다** — 같은 배치의 형제 파일이 헤더를 넣어서 컴파일된다. unity를 끄면 빌드가 깨지고, 나중에 발견되면 수십 파일이 동시에 터진다

**따라서 두 모드를 모두 유지한다:**
```
로컬 / 릴리즈 : CMAKE_UNITY_BUILD=ON   (속도)
CI 게이트     : CMAKE_UNITY_BUILD=OFF  (누락 include · ODR 검출)
```
🔴 이 게이트가 없으면 대가 4번은 반드시 터진다.

**PCH 병용** — Boost.asio는 헤더가 무거워 PCH의 효율이 크다. 🔴 다만 **asio PCH는 `atlas_core`가 아니라 `atlas_net`에 붙는다** (실측 wp5): 코어는 asio에 의존하지 않는 것이 `types.h`를 싸게 유지하는 조건이고(`cpp-style.md §4.2`), asio를 코어 PCH에 넣으면 코어를 링크하는 모든 타깃이 asio를 파싱하게 되어 그 규칙이 무의미해진다.
```cmake
target_precompile_headers(atlas_core PRIVATE <string> <vector> <memory>)
target_precompile_headers(atlas_net  PRIVATE <boost/asio.hpp>)
```
안정적인 외부 헤더는 PCH로, 자주 바뀌지 않는 모듈은 unity로. 이 조합이 증분 악화를 최소화하면서 전체 빌드를 줄인다.

🔴 **`_WIN32_WINNT=0x0A00`을 `atlas_net`이 `PUBLIC`으로 정의한다.** 없으면 Boost.Asio가
`Assuming _WIN32_WINNT=0x0601 (Windows 7)`을 출력하고 실제로 Windows 7 API 표면으로 컴파일된다 —
dev(Windows)와 prod(Linux)가 서로 다른 소켓 구현을 쓰게 되는 조용한 분기다. `net_types.h`가
`<boost/asio.hpp>`를 모든 소비자에게 끌고 가므로 값이 타깃마다 달라지면 ODR 위반이 되어 `PUBLIC`이다.

### 15.2 의존성 — vcpkg manifest 모드

의존성은 3개다: **Boost(asio/beast) · OpenSSL · MySQL client** (+ spdlog, gtest).

```json
{ "dependencies": ["boost-asio","boost-beast","openssl","libmariadb","spdlog","gtest"],
  "builtin-baseline": "eaca4a577b6b678c6e10252754b6988a61746c19" }
```

🔴 **MySQL client = `libmariadb`다(2026-08-10 교체, 근거 §15.5).** MariaDB Connector/C 는 MySQL
와이어 프로토콜과 C API(`mysql.h`) 양쪽 호환이므로 **접속 대상 서버는 여전히 MySQL이고**(§15.3
compose의 `mysql` 서비스는 그대로다), 바뀐 것은 클라이언트 라이브러리 하나뿐이다.

- **`gtest`** — 테스트 프레임워크는 GoogleTest로 확정(2026-08-06). `gtest_discover_tests`로 §15.4의
  `test` 단계가 ctest에 결선된다.
- 🔴 **`builtin-baseline` SHA는 손으로 적지 않는다.** `server/vcpkg.json`은 빈 값으로 시작하고,
  `server/setup.bat`이 vcpkg를 클론한 직후 `git rev-parse HEAD`의 실측값을 써 넣는다
  (`server/scripts/set-vcpkg-baseline.ps1`). 지어낸 SHA는 재현성의 반대다.
  위 값은 그렇게 채워진 실측 SHA다(2026-08-06 최초 `setup.bat` 실행 결과). 이 baseline에서
  해석된 포트 버전: Boost 1.91.0 · OpenSSL 3.6.3 · spdlog 1.17.0 · gtest 1.17.0 ·
  **libmariadb 3.4.8** (`libmariadb[core,iconv]:x64-windows@3.4.8` — 2026-08-10 교체 후
  `cmake --preset windows-debug` 실측. 🔴 `libmysql` 8.0.46 이 이 자리에 있었다).
  CMake 소비 형태도 바뀐다: `find_package(unofficial-libmariadb CONFIG REQUIRED)` →
  `unofficial::libmariadb`.
  갱신은 vcpkg를 fetch 한 뒤 같은 스크립트를 다시 돌려서 하고, 손으로 고치지 않는다.

- **baseline SHA 고정** → 재현 가능. classic 모드(전역 설치)의 "어제는 됐는데" 문제가 사라진다
- **로컬 바이너리 캐시는 기본 활성**이다(`%LOCALAPPDATA%\vcpkg\archives`). 두 번째 빌드부터는 Boost를 다시 컴파일하지 않는다
- 🔴 **진짜 문제는 Docker/CI다** — 컨테이너는 매번 새 파일시스템이라 캐시가 남지 않는다. 명시적으로 뚫는다:
```dockerfile
RUN --mount=type=cache,target=/root/.cache/vcpkg/archives \
    cmake --preset linux-release && cmake --build --preset linux-release
```
또는 원격 바이너리 소스(`VCPKG_BINARY_SOURCES`).

**왜 Conan이 아닌가**: 의존성 3개로는 Conan의 강점(복잡한 의존 그래프 해결)이 발휘될 자리가 없고, 학습할 개념이 더 많다. 이미 Conan에 익숙하다면 그쪽도 정답이다.

🔴 **왜 `apt install libboost-all-dev`가 아닌가**: dev(Windows)와 prod(Linux)의 Boost 버전이 갈린다. asio는 버전 간 동작 차이가 실재하고, **dev/prod 의존성 버전 동일 보장**이 이 프로젝트에서 실제로 중요하다(자매 프로젝트에서 겪은 "Docker 레이어 캐시 = 옛 바이너리" 함정과 같은 계열의 사고다).

### 15.3 배포

- Windows 개발 / Linux Docker 배포, CMake 크로스
- C++ 멀티스테이지 Dockerfile (빌더 스테이지 + 런타임 스테이지)
- 🔴 **Docker 레이어 캐시로 옛 바이너리가 배포되는 사고**를 경계한다 — 배포 후 버전 확인을 검증 절차에 포함한다

🔴 **CD는 Phase 1 비범위다**(§3.4). 레지스트리 푸시 · 무중단 롤아웃 · 배포 자동화를 만들지 않는다. 이 절이 정하는 것은 **`compose` 로 로컬 / 단일 호스트를 띄우는 데까지**다.

**compose 구성 — 프로파일로 기본 기동을 최소화한다.**

| 서비스 | 프로파일 | 기동 |
|---|---|---|
| `mysql` | 기본 | 항상 — ORM(§10)의 유일한 필수 런타임 의존 |
| `redis` | 프로파일 뒤 | 🔴 기본 기동 제외. 정의만 두고 쓰지 않는다(§10.2) |
| `fe` · `game` · `world` | 프로파일 뒤 | 서버 바이너리가 생기는 시점에 켠다 |

**단일 이미지 + `ATLAS_ROLE` 분기 entrypoint.** 서버 3종을 각각의 이미지로 굽지 않는다. 이미지는 하나이고, entrypoint가 `ATLAS_ROLE` 환경변수로 실행 대상을 고른다. 이것이 §5.2의 "WORLD와 InterWorld는 동일 바이너리, ini로 식별"과 정합한 배포 형태다 — 배포 단위를 role별로 쪼개면 그 명제가 이미지 층에서 깨진다.

🔴 FE / GAME / WORLD 바이너리는 **아직 없다.** 그러므로 entrypoint가 지금 하는 일은 **role → 바이너리 경로 매핑과, 바이너리가 없을 때의 명확한 실패**까지다. 없는 것을 있는 척 감싸지 않는 절제이며, §10.1이 "런타임 없이 CRUD를 뽑지 않는다"로 취한 것과 같은 종류의 판단이다.

### 15.4 CI 게이트

```
gen:check → core-purity → format-check → clang-tidy → build(unity ON) → build(unity OFF) → test
```

🔴 **`gen:check`가 맨 앞에 온다.** "생성 출력을 직접 편집하지 않는다"는 규칙의 유일한 기계 강제다 —
`server/generated/**`가 현재 입력(`shared/contracts` · `server/db/schema.json`)으로부터 재생성한
결과와 다르면 여기서 멈춘다. 문서로만 있으면 반드시 손으로 고친 생성물이 커밋된다.

🔴 **`core-purity` 가 그 바로 뒤에 온다 — "코어가 게임을 알면 안 된다"의 기계 강제다.**
§14는 코어(고정) / 계약(생성) / 게임(교체) 층을 선언하지만, 지금까지 그 경계를 강제하는 것이
**아무것도 없었다.** `gen:check` 가 생성물 손편집에 대해 하는 일을, 이 단계가 코어 오염에 대해 한다.
검사 2가지다.

- `server/atlas/**` 가 `server/generated/**` 의 **게임 계약 헤더를 include 하지 않는다**
- `server/atlas/**` 에 `tools/core_purity/denylist.txt` 의 용어(§3.3 데모 게임 어휘)가 **0건**

🔴 denylist는 고정 목록이 아니다 — **데모 게임이 자라면 denylist도 같이 자란다.** §3.3에 항목을
추가할 때 그 어휘를 denylist에 넣지 않으면, 이 게이트는 조용히 낡은 어휘만 지키는 껍데기가 된다.

구현: `server/scripts/ci-gate.ps1` (git remote가 붙기 전까지 **이것이 실질 게이트**) 와
`.github/workflows/ci.yml` (Linux 전사본, 아직 미검증).
`configure`는 게이트 단계가 아니라 준비 단계로 format-check 앞에 들어간다 — clang-tidy가 요구하는
`compile_commands.json`은 구성된 Ninja 트리에서만 나오기 때문이다.

🔴 **`clang-tidy` 단계만 두 구현이 동등하지 않다 — CI(Linux/clang) 전용이다.** 로컬 Windows 게이트는
이 단계를 건너뛴다(조용히가 아니라 사유를 출력하고 건너뛴다). 실측 2026-08-06: MSVC로 구성한
`compile_commands.json`에 대해 clang-tidy 19.1.5가 CMake PCH 산출물에서 멈춘다 —
`cmake_pch.cxx.pch` 는 MSVC `/Yc` 바이너리라 clang이 읽지 못한다
(`is not a valid precompiled PCH file: file doesn't start with AST file magic`).
PCH만 빼면 같은 호출이 `--warnings-as-errors=*` 에서 exit 0 이므로 비호환은 PCH 하나로 한정된다.
🔴 그렇다고 `/Y-` 를 끼워 넣지 않는다 — 컴파일러가 실제로 빌드하지 않는 TU를 검사하게 되어
게이트가 거짓 신호를 준다. 해소 조건은 로컬 clang-cl 도입(§15.1 툴체인)이며, 그 전까지
네이밍·`bugprone`·`modernize` 강제는 `linux-ci` 에서만 성립한다. 상세는 `cpp-style.md §7.3`.

### 15.5 CI 인프라 — vcpkg 바이너리 캐시 (2026-08-07 실측)

첫 CI 런(run 31142102019)이 **실패**했고, 재실행도 **실패**했다. 둘 다 `configure` 단계이며 원인은
레포 버그가 아니라 **업스트림 아카이브 장애**다. 🔴 두 런이 **서로 다른 포트에서 죽었다** — 일시적
장애가 아니라 구조 문제라는 증거다.

1회차:
```
error: download from .../ncurses-6.5.tar.gz had an unexpected hash
error: curl operation failed with response code 500.
error: Reached maximum number of attempts, won't retry download from
       https://github.com/boostorg/polygon/archive/boost-1.91.0.tar.gz
error: building boost-polygon:x64-linux failed with: BUILD_FAILED
```
2회차(같은 워크플로, 다른 포트):
```
error: Reached maximum number of attempts, won't retry download from
       https://github.com/boostorg/core/archive/boost-1.91.0.tar.gz
error: building boost-core:x64-linux failed with: BUILD_FAILED
```

의존성 트리 실측(`vcpkg depend-info <port> --format=list`, 로컬 vcpkg — 🔴 출력은 stderr로 나온다):

| 포트 | 의존 포트 수 |
|---|---|
| **`libmysql`** | **92** ← Boost 트리 전체 + `ncurses` 를 여기서 끌고 온다 |
| `boost-beast` | 60 |
| `boost-asio` | 55 (beast는 asio 대비 +5뿐) |
| `libmariadb` | 4 |
| `openssl` · `spdlog` | 각 4 |
| `gtest` | 3 |

🔴 CI 로그의 `boost-polygon` · `boost-bimap` · `ncurses` 는 **전부 `libmysql` 경유**다. `boost-beast`
는 원인이 아니다.

구조 문제는 셋이다.
1. 합집합 ~100개 포트를 **매 런 소스 빌드**한다
2. `actions/cache` 로 `~/.cache/vcpkg/archives` 를 감싸는 방식은 첫 런 미스 + 실패 종료로 캐시가
   차지 않았다 → vcpkg **GHA 바이너리 캐시**(`VCPKG_BINARY_SOURCES=clear;x-gha,readwrite`)로 전환한다
3. 소스 ~100개 중 하나만 500을 뱉으면 게이트 전체가 죽는다 — **재시도 계층이 없다**

**확정 (2026-08-07): 캐시 전환 + `configure` 재시도만 한다.** `server/vcpkg.json` 은 **수정하지
않는다**(6개 유지, §15.2).

🔴 `libmysql` → `libmariadb` 교체는 **채택하지 않았다.** 포트가 ~100 → ~62로 줄지만 `boost-asio` 의
55개가 그대로 남아 `boostorg` 500 위험은 사라지지 않고, 캐시가 워밍되면 그 이득은 두 번째 런부터
0이 된다. DB 클라이언트 라이브러리 교체와 교환하기에는 비율이 나쁘다. 다만 **ORM 런타임 노드 착수
전에 별건으로 재검토할 항목**으로 §16 미결 표에 남긴다(빌드 시간 · 런타임 이미지 크기 · `ncurses`
불안정 미러 제거가 근거).

#### 15.5a 3차 실패와 결정 번복 — `libmariadb` 교체 (2026-08-10 확정)

위 확정을 적용한 런(31153396554, 커밋 `9529f1d`)도 `configure` 에서 **3/3 실패**했다. 🔴 이번 실패는
**플레이크가 아니다** — 재시도해도 같은 자리에서 같은 이유로 죽는 결정적 실패다.

```
CMake Error at cmake/rpc.cmake:113 (MESSAGE):
  Could not find rpc/rpc.h in /usr/include or /usr/include/tirpc
Call Stack (most recent call first):
  CMakeLists.txt:2093 (MYSQL_CHECK_RPC)
-- Running vcpkg install - failed
```

ubuntu-24.04 는 glibc에서 Sun RPC 헤더를 제거했고, `libmysql` 은 그것을 요구한다. 필요한 것은
`libtirpc-dev` 다.

🔴 **그런데 이 사실은 이미 레포 안에 있었다.** `server/Dockerfile` 은 `libtirpc-dev libncurses-dev`
를 설치하면서 *바로 그 에러 메시지를* 주석으로 적어두고 있었다. `.github/workflows/ci.yml` 만
몰랐다. **같은 플랫폼을 빌드하는 두 레시피가 갈라져 있었던 것**이 진짜 결함이고, 이런 종류의 분기는
DB 클라이언트가 시스템 전제조건을 요구하는 한 계속 재발한다.

**세 번의 실패, 세 가지 원인, 하나의 공통 출처(`libmysql`):**

| 런 | 죽은 지점 | 성격 |
|---|---|---|
| 31142102019 | `boost-polygon` 500 + `ncurses` 해시 불일치 | 업스트림 플레이크 — 둘 다 `libmysql` 경유 |
| (재실행) | `boost-core` 500 | 업스트림 플레이크 — `libmysql` 경유 |
| 31153396554 | `rpc/rpc.h` 없음 | **결정적** — `libmysql` 자체의 시스템 전제조건 |

🔴 **2026-08-07의 판단을 뒤집는다.** 당시 근거("이득이 캐시 워밍 후 0이 된다")는 실패가 *업스트림
다운로드 플레이크뿐*이라는 전제 위에 있었다. 3차 실패가 그 전제를 깼다 — 캐시는 시스템 전제조건
누락을 고쳐주지 않는다. 92개 포트를 끌고 오는 의존성은 표면적이 넓고, 그 표면적이 세 번 연속으로
게이트를 죽였다.

**확정: `libmysql` → `libmariadb`.** 의존 포트 92 → 4. 근거 4가지:

1. **결정적 실패의 제거** — `libmariadb` 는 Sun RPC 도 `ncurses` 도 요구하지 않는다. `libtirpc-dev`
   를 한 줄 더 붙이는 것은 증상만 덮는 것이고, 다음 시스템 전제조건에서 같은 사고가 반복된다
2. **`ncurses` 불안정 미러가 의존 그래프에서 사라진다** — 1차 실패의 해시 불일치 출처
3. **와이어 호환** — MariaDB Connector/C 는 MySQL 서버에 그대로 붙는다. §15.3 compose의 `mysql`
   서비스도, §10의 ORM 설계도, `db/schema.json` 도 바뀌지 않는다. 바뀌는 것은 링크되는 .so 하나다
4. **지금이 §16이 지정한 그 시점이다** — 미결 표는 이 항목의 판단 시점을 "ORM 런타임 노드 착수 시"로
   적어뒀고, ORM 런타임 노드가 지금 착수된다

🔴 **`boost-asio` 의 55개 포트는 여전히 남는다.** 이 교체는 `boostorg` 500 위험을 없애지 않는다 —
그 위험에 대한 대응은 §15.5의 GHA 바이너리 캐시 + `configure` 재시도이며, 그 둘은 유지된다. 이
절이 없애는 것은 **`libmysql` 고유의 표면적**이지 업스트림 장애 일반이 아니다. 과대평가하지 않는다.

동반 변경: `server/Dockerfile` 에서 `libtirpc-dev libncurses-dev` 를 제거한다(존재 이유가
`libmysql` 하나였다). `server/vcpkg.json` 의 의존성 개수는 6개 그대로다.

**로컬 실측 (2026-08-10, `cmake --preset windows-debug`)**: `configure` 성공.
`libmariadb[core,iconv]:x64-windows@3.4.8` 설치에 **21초** — `libmysql` 의 92포트 소스 빌드와
같은 자리다. `-- Configuring done (33.6s)`.

🔴 **그러나 CI 는 아직 검증되지 않았다.** 위 실측은 Windows 로컬이고, 실패한 것은 Linux 러너다.
그리고 `configure` 이후 단계(format-check · clang-tidy · build ×2 · test)는 러너에서 **한 번도
실행된 적이 없다**. 이 교체가 `configure` 를 통과시키더라도 그 뒤에서 처음 드러나는 실패가 남아
있을 수 있다. 초록 런 id 를 여기 적기 전까지 이 절은 "고쳤다"가 아니라 **"원인을 제거하고 다음
실패를 노출시키기로 했다"**이다.

---

## 16. 미결 사항

지금 정하지 않으며, 정할 시점을 함께 적는다. 🔴 근거 없이 미리 정하지 않는다.

| 항목 | 정할 시점 |
|---|---|
| p95 틱 처리 시간 · 대역폭 목표치 | 1차 부하 측정 후 역산 |
| PC 클라이언트 배포처 | Phase 3 |
| 게임 타이틀 | Phase 1 후반 (공개 레포명은 `project-atlas`로 확정됐다 — §2. 타이틀은 레포명과 별개 항목이다) |
| HMAC vs AEAD 최종 선택 | **여전히 열려 있다.** §8.1 이 헤더를 12바이트로 확정하면서 무결성 2층 필드를 **의도적으로 넣지 않았다**(자리만 잡는 것을 거부). 클라이언트가 없는 동안 헤더 확장 비용은 0이므로, 정할 시점은 §12 platform-auth 연동으로 세션키가 실재하게 되는 때다 |
| WORLD 틱 레이트 | 데모 게임 조작감 확인 후 |
| 세마포어 적용 지점 | Phase 2 (Admin HTTP 계층) |
| ~~`libmysql` → `libmariadb` 교체 여부~~ | **해소됨 (2026-08-10) — 교체 확정, §15.5a** |

---

## 17. 리스크

| 리스크 | 성격 | 완화 |
|---|---|---|
| **범위 폭발** | 이 코어 목록은 상용 게임서버 한 벌이다. 팀이 6개월~1년 걸리는 분량을 1인이 구직과 병행한다 | 데모 게임을 §3.3에서 절대 늘리지 않는다. 코어가 산출물이고 게임은 하네스다 |
| **프레임워크 검증 부족** | 게임 1개로는 경계가 검증되지 않는다 | 추상화를 미리 하지 않고 seam을 generator에만 둔다. Phase 2에서 기존 게임 1종 이식 |
| **C++ 신규 스택** | 학습 비용이 일정에 들어있지 않다 | Boost.asio + OpenSSL + MySQL C API로 범위를 좁힌다. 나머지는 표준 라이브러리 |
| **구직과의 캐파 경합** | 구직이 1순위다 | 이 프로젝트가 구직 산출물이라는 정렬을 유지한다. 게임 완성도로 목표가 이동하면 정렬이 깨진다 |
| **JWKS C++ 구현 저평가** | OpenSSL 직접 작업의 분량을 작게 잡기 쉽다 | 초기 스파이크로 실측 후 일정 반영 |

---

## 18. 참조

- `docs/conventions/cpp-style.md` — 코딩 컨벤션 SoT (네임스페이스 · 네이밍 · 타입 · 매크로 · 템플릿 · 강제 구조)
