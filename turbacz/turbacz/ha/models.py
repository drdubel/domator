"""Pydantic models for the Home Assistant bridge configuration."""

from enum import Enum
from typing import Optional

from pydantic import BaseModel, Field


class CapabilityType(str, Enum):
    light = "light"
    cover = "cover"
    climate = "climate"


class HACapability(BaseModel):
    id: str = Field(description="Stable unique identifier (UUID)")
    device_id: str
    capability_type: CapabilityType
    name: str


class HADevice(BaseModel):
    id: str = Field(description="Stable unique identifier (UUID)")
    area_id: str
    name: str
    capabilities: list[HACapability] = []


class HAArea(BaseModel):
    id: str = Field(description="Stable unique identifier (UUID)")
    home_id: str
    name: str
    devices: list[HADevice] = []


class HAHome(BaseModel):
    id: str = Field(description="Stable unique identifier (UUID)")
    name: str
    areas: list[HAArea] = []


class HACapabilityCreate(BaseModel):
    device_id: str
    capability_type: CapabilityType
    name: str


class HADeviceCreate(BaseModel):
    area_id: str
    name: str


class HAAreaCreate(BaseModel):
    home_id: str
    name: str


class HAHomeCreate(BaseModel):
    name: str


class HAApplyResult(BaseModel):
    published: list[str] = Field(default_factory=list, description="Discovery topics published")
    removed: list[str] = Field(default_factory=list, description="Discovery topics cleared (entity removed)")
    errors: list[str] = Field(default_factory=list, description="Non-fatal errors encountered")
