# V11 confirmed blocker and v12 response

The real v11 laptop reports show that RimWorld reached menu logic: music and menu hover sounds worked, the process remained responsive, and the renderer loaded from the v11 runtime folder. The engine failed loading `Resources/Textures/UI/HeroArt/MenuBG_Ideology.jpg`: 5333×3000 BC7 UNORM with 13 mips. The bridge's hidden-mip backing became 21332×12000, beyond D3D11's 16384 axis limit, and returned ErrorFormatNotSupported. This interrupted the menu background drawing path. No device-loss marker was observed in that inspected run.

V12 addresses that specific initiating failure with representation selection and decoded upload/view handling, not by removing the native size guard or pretending BC7 backing succeeded. The exact shape now passes a strict pixel/mip/upload regression in the sandbox. This is necessary progress; it does not prove that the next native game run will have no further defect.

V11's compact atlas fix remains: the real run previously peaked near 38.99 MiB tracked compact scratch and returned that category to zero on release. V12 does not undo it or promise a universal memory cap. The decoded background has higher byte cost than logical BC storage, documented separately in the v12 memory trace.

Use the whole v12 package in a new folder. The launcher now captures representation dimensions and classifies repeated fresh rendering errors as a failed test even if native exit code is 0. The next laptop report is the required test of actual menu pixels and any subsequent blocker.
