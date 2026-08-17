import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mapa_parada as mp


class MapStoppingTests(unittest.TestCase):
    def test_default_plan_is_gradual_and_bounded(self):
        plan = mp.default_plan()
        self.assertEqual(plan[0], (1.0, 50.0))
        self.assertEqual(plan[-1], (20.0, 4.0))
        self.assertEqual(len(plan), 24)
        self.assertTrue(all(1 <= speed <= 20 and 4 <= course <= 50 for speed, course in plan))
        for speed in mp.DEFAULT_SPEEDS:
            self.assertEqual(
                [course for current, course in plan if current == speed],
                [50, 30, 15, 4],
            )

    def test_report_prefers_simpler_model_for_linear_response(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            for condition, (speed, course) in enumerate(((2.0, 15.0), (5.0, 30.0), (10.0, 50.0)), 1):
                folder = root / f"{condition:02d}_v{speed}_c{course}"
                dev = folder / "DadosDev"
                dev.mkdir(parents=True)
                (folder / "condition_result.json").write_text(json.dumps({"accepted": True}))
                path = dev / "recip_stop_diagnostics.csv"
                path.write_text(
                    "stroke;sentido;curso_configurado_mm;velocidade_externa_modulo_mm_s;distancia_parada_apos_gatilho_legado_mm\n"
                    + "\n".join(
                        f"{stroke};{1 if stroke % 2 else -1};{course};{speed};{0.1 * speed}"
                        for stroke in range(1, 9)
                    )
                    + "\n",
                    encoding="ascii",
                )
            payload = mp.generate_report(root)
            self.assertIsNotNone(payload)
            self.assertEqual(payload["raw_stop_points"], 18)
            self.assertEqual(payload["balanced_model_points"], 6)
            self.assertEqual(payload["selected"]["name"], "tempo_resposta")
            self.assertTrue((root / "RELATORIO_MAPA_PARADA.md").is_file())

    def test_command_line_requires_explicit_motion_confirmation(self):
        with mock.patch.object(
            sys, "argv", ["mapa_parada.py", "--speed", "1", "--course", "50"]
        ):
            with self.assertRaises(SystemExit) as error:
                mp.main()
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
