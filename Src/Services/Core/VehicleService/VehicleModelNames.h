#pragma once

#include <array>
#include <string>
#include <string_view>

// Каталог имён ВСЕХ моделей машин GTA SA (400..611) — стандартные английские
// названия SA-MP. Справочные данные: lookup по модели за O(1) (индекс model-400),
// string_view смотрит в статические литералы — ни аллокаций, ни копий.
// Модель может прийти из клиентских данных — вне диапазона отвечаем пустым
// string_view, не UB.
//
// Осознанные отступления от стокового SA-MP-списка (при сверке НЕ «чинить»
// обратно, см. Docs/Vehicles.md): 604/605 — в стоке "Glendale Shit"/"Sadler
// Shit" (утёкшие отладочные имена, мат игроку не печатаем) -> "... Beater";
// 435 — "Article Trailer 1" (единый ряд с 450 "... 2" и 591 "... 3").
namespace VehicleModelNames
{
inline constexpr int MIN_MODEL = 400;
inline constexpr int MAX_MODEL = 611;

// Индекс = model - MIN_MODEL. inline: таблицу odr-использует inline get() —
// без external linkage каждая TU получала бы свой экземпляр (формальный ODR).
inline constexpr std::array<std::string_view, MAX_MODEL - MIN_MODEL + 1> TABLE = {
    "Landstalker",        // 400
    "Bravura",            // 401
    "Buffalo",            // 402
    "Linerunner",         // 403
    "Perennial",          // 404
    "Sentinel",           // 405
    "Dumper",             // 406
    "Firetruck",          // 407
    "Trashmaster",        // 408
    "Stretch",            // 409
    "Manana",             // 410
    "Infernus",           // 411
    "Voodoo",             // 412
    "Pony",               // 413
    "Mule",               // 414
    "Cheetah",            // 415
    "Ambulance",          // 416
    "Leviathan",          // 417
    "Moonbeam",           // 418
    "Esperanto",          // 419
    "Taxi",               // 420
    "Washington",         // 421
    "Bobcat",             // 422
    "Mr Whoopee",         // 423
    "BF Injection",       // 424
    "Hunter",             // 425
    "Premier",            // 426
    "Enforcer",           // 427
    "Securicar",          // 428
    "Banshee",            // 429
    "Predator",           // 430
    "Bus",                // 431
    "Rhino",              // 432
    "Barracks",           // 433
    "Hotknife",           // 434
    "Article Trailer 1",  // 435
    "Previon",            // 436
    "Coach",              // 437
    "Cabbie",             // 438
    "Stallion",           // 439
    "Rumpo",              // 440
    "RC Bandit",          // 441
    "Romero",             // 442
    "Packer",             // 443
    "Monster",            // 444
    "Admiral",            // 445
    "Squalo",             // 446
    "Seasparrow",         // 447
    "Pizzaboy",           // 448
    "Tram",               // 449
    "Article Trailer 2",  // 450
    "Turismo",            // 451
    "Speeder",            // 452
    "Reefer",             // 453
    "Tropic",             // 454
    "Flatbed",            // 455
    "Yankee",             // 456
    "Caddy",              // 457
    "Solair",             // 458
    "Berkley's RC Van",   // 459
    "Skimmer",            // 460
    "PCJ-600",            // 461
    "Faggio",             // 462
    "Freeway",            // 463
    "RC Baron",           // 464
    "RC Raider",          // 465
    "Glendale",           // 466
    "Oceanic",            // 467
    "Sanchez",            // 468
    "Sparrow",            // 469
    "Patriot",            // 470
    "Quad",               // 471
    "Coastguard",         // 472
    "Dinghy",             // 473
    "Hermes",             // 474
    "Sabre",              // 475
    "Rustler",            // 476
    "ZR-350",             // 477
    "Walton",             // 478
    "Regina",             // 479
    "Comet",              // 480
    "BMX",                // 481
    "Burrito",            // 482
    "Camper",             // 483
    "Marquis",            // 484
    "Baggage",            // 485
    "Dozer",              // 486
    "Maverick",           // 487
    "News Chopper",       // 488
    "Rancher",            // 489
    "FBI Rancher",        // 490
    "Virgo",              // 491
    "Greenwood",          // 492
    "Jetmax",             // 493
    "Hotring Racer",      // 494
    "Sandking",           // 495
    "Blista Compact",     // 496
    "Police Maverick",    // 497
    "Boxville",           // 498
    "Benson",             // 499
    "Mesa",               // 500
    "RC Goblin",          // 501
    "Hotring Racer A",    // 502
    "Hotring Racer B",    // 503
    "Bloodring Banger",   // 504
    "Rancher Lure",       // 505
    "Super GT",           // 506
    "Elegant",            // 507
    "Journey",            // 508
    "Bike",               // 509
    "Mountain Bike",      // 510
    "Beagle",             // 511
    "Cropduster",         // 512
    "Stuntplane",         // 513
    "Tanker",             // 514
    "Roadtrain",          // 515
    "Nebula",             // 516
    "Majestic",           // 517
    "Buccaneer",          // 518
    "Shamal",             // 519
    "Hydra",              // 520
    "FCR-900",            // 521
    "NRG-500",            // 522
    "HPV1000",            // 523
    "Cement Truck",       // 524
    "Towtruck",           // 525
    "Fortune",            // 526
    "Cadrona",            // 527
    "FBI Truck",          // 528
    "Willard",            // 529
    "Forklift",           // 530
    "Tractor",            // 531
    "Combine Harvester",  // 532
    "Feltzer",            // 533
    "Remington",          // 534
    "Slamvan",            // 535
    "Blade",              // 536
    "Freight",            // 537
    "Brown Streak",       // 538
    "Vortex",             // 539
    "Vincent",            // 540
    "Bullet",             // 541
    "Clover",             // 542
    "Sadler",             // 543
    "Firetruck LA",       // 544
    "Hustler",            // 545
    "Intruder",           // 546
    "Primo",              // 547
    "Cargobob",           // 548
    "Tampa",              // 549
    "Sunrise",            // 550
    "Merit",              // 551
    "Utility Van",        // 552
    "Nevada",             // 553
    "Yosemite",           // 554
    "Windsor",            // 555
    "Monster A",          // 556
    "Monster B",          // 557
    "Uranus",             // 558
    "Jester",             // 559
    "Sultan",             // 560
    "Stratum",            // 561
    "Elegy",              // 562
    "Raindance",          // 563
    "RC Tiger",           // 564
    "Flash",              // 565
    "Tahoma",             // 566
    "Savanna",            // 567
    "Bandito",            // 568
    "Freight Flat",       // 569
    "Streak Carriage",    // 570
    "Kart",               // 571
    "Mower",              // 572
    "Duneride",           // 573
    "Sweeper",            // 574
    "Broadway",           // 575
    "Tornado",            // 576
    "AT-400",             // 577
    "DFT-30",             // 578
    "Huntley",            // 579
    "Stafford",           // 580
    "BF-400",             // 581
    "Newsvan",            // 582
    "Tug",                // 583
    "Petrol Trailer",     // 584
    "Emperor",            // 585
    "Wayfarer",           // 586
    "Euros",              // 587
    "Hotdog",             // 588
    "Club",               // 589
    "Freight Carriage",   // 590
    "Article Trailer 3",  // 591
    "Andromada",          // 592
    "Dodo",               // 593
    "RC Cam",             // 594
    "Launch",             // 595
    "Police Car (LSPD)",  // 596
    "Police Car (SFPD)",  // 597
    "Police Car (LVPD)",  // 598
    "Police Ranger",      // 599
    "Picador",            // 600
    "S.W.A.T. Van",       // 601
    "Alpha",              // 602
    "Phoenix",            // 603
    "Glendale Beater",    // 604 (сток "Glendale Shit" — мат, см. шапку)
    "Sadler Beater",      // 605 (сток "Sadler Shit" — мат, см. шапку)
    "Baggage Trailer A",  // 606
    "Baggage Trailer B",  // 607
    "Tug Stairs Trailer", // 608
    "Boxville Mission",   // 609
    "Farm Trailer",       // 610
    "Utility Trailer",    // 611
};

// Имя модели; пустой string_view для модели вне 400..611 (модель может прийти
// из клиентских данных — не краш и не UB).
constexpr std::string_view get(int model)
{
    if (model < MIN_MODEL || model > MAX_MODEL)
        return {};
    return TABLE[model - MIN_MODEL];
}

// Имя модели для игроко-видимого текста: имя каталога, а при пустом (модель вне
// 400..611 — битые данные) — фолбэк "Модель {id}" (пустую строку игроку не
// показываем). utf-8, как прочие игроковые строки, — вывод оборачивает u() на месте.
inline std::string displayName(int model)
{
    const std::string_view name = get(model);
    if (!name.empty())
        return std::string(name);
    return "Модель " + std::to_string(model);
}
} // namespace VehicleModelNames
