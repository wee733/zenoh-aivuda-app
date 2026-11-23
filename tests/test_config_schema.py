"""Offline config-panel compatibility tests; requires PyYAML and jsonschema."""
import copy
from pathlib import Path
import unittest

from jsonschema import Draft7Validator
import yaml


class DomainSchemaTest(unittest.TestCase):
    def test_upgrade_layouts(self):
        config = Path(__file__).resolve().parents[1] / 'config'
        validator = Draft7Validator(yaml.safe_load((config / 'config_schema.yaml').read_text()))
        current = yaml.safe_load((config / 'default_config.yaml').read_text())
        legacy = copy.deepcopy(current)
        del legacy['zenoh']
        legacy['ros']['domain_id'] = 42
        mixed = {**legacy, 'zenoh': {'ros_domain_id': -1}}
        validator.validate(current)
        for settings in (legacy, mixed):
            with self.subTest(settings=settings):
                self.assertFalse(validator.is_valid(settings))
        current['zenoh']['ros_domain_id'] = 233
        self.assertFalse(validator.is_valid(current))


if __name__ == '__main__':
    unittest.main()
