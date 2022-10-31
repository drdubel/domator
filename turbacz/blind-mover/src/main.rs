#![deny(unsafe_code)]
#![no_main]
#![no_std]

use panic_halt as _;

use nb::block;

use numtoa::NumToA;

use cortex_m_rt::entry;
use stm32f1xx_hal::{
    pac,
    prelude::*,
	serial::{Config, Serial, Tx3}, gpio::{PushPull, Floating},
};

struct Blind {
	pin_a: PushPull,
	pin_b: PushPull,
	position: u16,
	target: u16
}

impl Blind {
	fn setup(&self, pin_a: PushPull, pin_b: PushPull) {
		self.pin_a = pin_a;
		self.pin_b = pin_b;
		self.position = 0;
		self.target = 999;
	}
	fn move_up(&self) {
		self.pin_a.set_high();
		self.pin_b.set_low();
	}
	fn move_down(&self) {
		self.pin_a.set_low();
		self.pin_b.set_high();
	}
	fn stop(&self) {
		self.pin_a.set_low();
		self.pin_b.set_low();
	}
}

fn blind_simulator(blind: u8, new_state: u16, act_state: &mut u16) {
    while new_state != *act_state {
        if new_state < *act_state {
            if *act_state - new_state < 5 {
                *act_state -= 1;
            }
            *act_state -= 5;
        } else if new_state > *act_state {
            if new_state - *act_state < 5 {
                *act_state += 1;
            }
            *act_state += 5;
        }
    }
}

fn move_blind(blind: u8, new_state: u16, act_state: &mut u16) {
	blind_simulator(blind, new_state, act_state);
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
    let mut gpiob = p.GPIOB.split(&mut rcc.apb2);
    let tx = gpiob.pb10.into_alternate_push_pull(&mut gpiob.crh);
    let rx = gpiob.pb1;

	 
	let x = gpiob.pb1.into_push_pull_output(&mut gpiob.crl);
	
	
	let sypialnia = Blind::setup(
		gpiob.pb1.into_push_pull_output(&mut gpiob.crl),
		gpiob.pb2.into_push_pull_output(&mut gpiob.crl)
	);

    let serial = Serial::usart3(
        p.USART3,
        (tx, rx),
        &mut afio.mapr,
        Config::default(),
        clocks,
        &mut rcc.apb1,
    );
    let (mut tx, mut rx) = serial.split();

    let mut blinds: [u16; 7] = [0, 0, 9, 0, 0, 0, 0];
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
