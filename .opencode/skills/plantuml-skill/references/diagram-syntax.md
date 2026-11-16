# Diagram syntax cookbook

Load the relevant example when generating or editing diagram syntax. Render and
validate through [the shared workflow](../SKILL.md#workflow).

## Component / Architecture Diagram

```plantuml
@startuml
!theme plain

title Microservices Architecture

actor "Client" as client
rectangle "API Gateway" as gateway #LightBlue

rectangle "Services" {
  component "User Service" as user
  component "Order Service" as order
}

database "User DB" as userdb
database "Order DB" as orderdb
queue "Kafka" as kafka

client --> gateway
gateway --> user
gateway --> order
user --> userdb
order --> orderdb
order --> kafka : events

@enduml
```

**Shape types:**
- `actor "Name" as id` — stick figure (user, external actor)
- `component "Name" as id` — component box with [brackets]
- `rectangle "Name" as id` — plain rectangle (for groups/layers)
- `database "Name" as id` — cylinder (database)
- `queue "Name" as id` — queue symbol
- `cloud "Name" as id` — cloud shape (external services)
- `node "Name" as id` — server/node box
- `frame "Name" as id` — frame grouping
- `package "Name" { }` — package grouping

**Arrows:**
- `A --> B` — solid arrow
- `A -> B` — thin arrow
- `A ..> B` — dashed arrow
- `A --> B : label` — labeled arrow
- `A <--> B` — bidirectional

**Colors:**
- `#LightBlue`, `#LightGreen`, `#LightYellow`, `#Pink`, `#Violet`
- `#AED6F1` (blue), `#A9DFBF` (green), `#FAD7A0` (orange), `#F1948A` (red)
- `#D7BDE2` (purple), `#F9E79F` (yellow), `#D3D3D3` (grey)

## Sequence Diagram

```plantuml
@startuml
!theme plain
title Login Flow

participant "Client" as C
participant "API Gateway" as G
participant "Auth Service" as A
database "User DB" as D

C -> G : POST /login
G -> A : validateCredentials(user, pass)
A -> D : SELECT * FROM users WHERE email = ?
D --> A : user record
A --> G : 200 OK + JWT token
G --> C : { token: "..." }

@enduml
```

**Arrow types:**
- `A -> B` — synchronous call
- `A --> B` — return / dashed
- `A ->> B` — async message
- `A -[#red]-> B` — colored arrow
- `activate A` / `deactivate A` — show activation box

## Class Diagram

```plantuml
@startuml
!theme plain

class User {
  +int id
  +String name
  +String email
  +login() : bool
  +logout()
}

class Order {
  +int id
  +Date createdAt
  +float total
  +place()
  +cancel()
}

class Product {
  +int id
  +String name
  +float price
}

User "1" --> "*" Order : places
Order "*" --> "*" Product : contains

@enduml
```

**Relationships:**
- `A --> B` — association
- `A --|> B` — inheritance
- `A ..|> B` — implements interface
- `A *-- B` — composition
- `A o-- B` — aggregation
- `A "1" --> "*" B : label` — with multiplicities

## ER Diagram

```plantuml
@startuml
!theme plain

entity "USER" as user {
  * id : int <<PK>>
  --
  name : varchar
  email : varchar
  created_at : datetime
}

entity "ORDER" as ord {
  * id : int <<PK>>
  --
  * user_id : int <<FK>>
  total : decimal
  status : varchar
}

entity "PRODUCT" as prod {
  * id : int <<PK>>
  --
  name : varchar
  price : decimal
}

user ||--o{ ord : places
ord }o--|{ prod : contains

@enduml
```

## Activity / Flowchart

```plantuml
@startuml
!theme plain

start

:Receive Order;

if (Payment valid?) then (yes)
  :Process Payment;
  :Send Confirmation Email;
  :Update Inventory;
  :Ship Order;
  :Mark as Delivered;
else (no)
  :Send Payment Failed Email;
  :Cancel Order;
endif

stop

@enduml
```

## State Diagram

```plantuml
@startuml
!theme plain

[*] --> Pending

Pending --> Processing : payment_received
Processing --> Shipped : packed
Shipped --> Delivered : confirmed
Processing --> Cancelled : cancel
Pending --> Cancelled : cancel

Delivered --> [*]
Cancelled --> [*]

@enduml
```

## C4 Context Diagram

C4 uses the bundled C4-PlantUML standard library via `!include <C4/...>`, which Kroki and recent local jars resolve with no network fetch. Export with the **standard** `plantuml` endpoint (the `c4plantuml` Kroki type also works).

```plantuml
@startuml
!include <C4/C4_Context>

title System Context — Internet Banking

Person(customer, "Banking Customer", "A customer of the bank")
System(banking, "Internet Banking System", "Lets customers view their accounts")
System_Ext(mail, "E-mail System", "The internal Microsoft Exchange system")

Rel(customer, banking, "Uses", "HTTPS")
Rel(banking, mail, "Sends e-mail via", "SMTP")
@enduml
```

Other levels: `<C4/C4_Container>` (`Container`, `ContainerDb`), `<C4/C4_Component>` (`Component`). Common macros: `Person`, `System`, `System_Ext`, `Container`, `Rel`, `Boundary`. **Do not** use a remote `!includeurl https://…` — Kroki cannot fetch external URLs; always use the bundled `<C4/…>` form.
