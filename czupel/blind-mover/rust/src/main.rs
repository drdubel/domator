#![deny(unsafe_code)]
#![no_main]
#![no_std]

use panic_halt as _;

use nb::block;

use numtoa::NumToA;

use cortex_m_rt::entry;
use embedded_hal::{digital::v2::OutputPin};
use stm32f1xx_hal::{
    pac,
    prelude::*,
    serial::{Config, Serial, Tx3}, delay,
};

struct Blind<PinUp, PinDown> {
    pin_up: PinUp,
    pin_down: PinDown,
    position: u16,
    target: u16,
}

impl<PinUp: OutputPin, PinDown: OutputPin> Blind<PinUp, PinDown> {
    fn setup(pin_up: PinUp, pin_down: PinDown) -> Blind<PinUp, PinDown> {
        Blind {
            pin_up: pin_up,
            pin_down: pin_down,
            position: 0,
            target: 999,
        }
    }
    fn move_up(&mut self) {
        self.pin_up.set_high().ok();
        self.pin_down.set_low().ok();
    }
    fn move_down(&mut self) {
        self.pin_up.set_low().ok();
        self.pin_down.set_high().ok();
    }
    fn stop(&mut self) {
        self.pin_up.set_low().ok();
        self.pin_down.set_low().ok();
    }
    fn set(&mut self, target: u16) {
        self.target = target;
        if self.position > target {
            self.move_down();
        } else if target < self.position {
            self.move_up();
        }
    }
}

fn give_state(blind: u8, new_state: u16, tx: &mut Tx3) {
    block!(tx.write(blind)).ok();
    let mut buf = [48u8; 5];

    new_state.numtoa(10, &mut buf);
    for n in 2..5 {
        block!(tx.write(buf[n])).ok();
    }
}

#[entry]
fn main() -> ! {
    let p = pac::Peripherals::take().unwrap();
    let mut flash = p.FLASH.constrain();
    let mut rcc = p.RCC.constrain();
    let clocks = rcc.cfgr.freeze(&mut flash.acr);
    let mut afio = p.AFIO.constrain(&mut rcc.apb2);
    let mut gpioa = p.GPIOA.split(&mut rcc.apb2);
    let mut gpiob = p.GPIOB.split(&mut rcc.apb2);

    let r1up = gpiob.pb12.into_push_pull_output(&mut gpiob.crh);
    let r1dn = gpiob.pb13.into_push_pull_output(&mut gpiob.crh);
    let r1 = Blind::setup(r1up, r1dn);

    let r2up = gpiob.pb14.into_push_pull_output(&mut gpiob.crh);
    let r2dn = gpiob.pb15.into_push_pull_output(&mut gpiob.crh);
    let r2 = Blind::setup(r2up, r2dn);

    let r3up = gpioa.pa8.into_push_pull_output(&mut gpioa.crh);
    let r3dn = gpioa.pa9.into_push_pull_output(&mut gpioa.crh);
    let r3 = Blind::setup(r3up, r3dn);

    let r4up = gpiob.pb6.into_push_pull_output(&mut gpiob.crl);
    let r4dn = gpiob.pb7.into_push_pull_output(&mut gpiob.crl);
    let r4 = Blind::setup(r4up, r4dn);

    let r5up = gpioa.pa6.into_push_pull_output(&mut gpioa.crl);
    let r5dn = gpioa.pa7.into_push_pull_output(&mut gpioa.crl);
    let r5 = Blind::setup(r5up, r5dn);

    let r6up = gpioa.pa4.into_push_pull_output(&mut gpioa.crl);
    let r6dn = gpioa.pa5.into_push_pull_output(&mut gpioa.crl);
    let r6 = Blind::setup(r6up, r6dn);

    let r7up = gpioa.pa2.into_push_pull_output(&mut gpioa.crl);
    let r7dn = gpioa.pa3.into_push_pull_output(&mut gpioa.crl);
    let r7 = Blind::setup(r7up, r7dn);

    let r8up = gpioa.pa0.into_push_pull_output(&mut gpioa.crl);
    let r8dn = gpioa.pa1.into_push_pull_output(&mut gpioa.crl);
    let r8 = Blind::setup(r8up, r8dn);

    let tx = gpiob.pb10.into_alternate_push_pull(&mut gpiob.crh);
    // Take ownership over pb11
    let rx = gpiob.pb11;

    let serial = Serial::usart3(
        p.USART3,
        (tx, rx),
        &mut afio.mapr,
        Config::default(),
        clocks,
        &mut rcc.apb1,
    );
    let (mut tx, mut rx) = serial.split();

    let mut blinds: [u16; 7] = [0, 0, 0, 0, 0, 0, 0];
    let mut blind = b'a';
    let mut new_state: u16 = 0;
    let mut digit: u32 = 2;

    loop {
        let received = block!(rx.read()).unwrap();
        if b'A' <= received && received <= b'G' {
            give_state(received + 32, blinds[usize::from(received) - 65], &mut tx);
        } else if b'a' <= received && received <= b'g' {
            blind = received;
            new_state = 0;
            digit = 2;
        }
        if b'1' <= received && received <= b'9' {
            new_state += u16::from(received) * 10_u16.pow(digit);
            if digit == 0 {
                move_blind(blind, new_state, &mut blinds[usize::from(blind)]);
            }
            digit -= 1;
        }
    }
}
